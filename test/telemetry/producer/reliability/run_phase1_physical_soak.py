#!/usr/bin/env python3
"""Run an opt-in physical Phase 1 soak against a real Freespace2 process.

All lifecycle control is supplied as JSON argv arrays.  This runner never
manufactures a game transition, and only reports transitions it observed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


SCHEMA = "fs2open.telemetry.phase1.physical-soak.v2"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def json_argv(value: str, label: str) -> list[str]:
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError as error:
        raise argparse.ArgumentTypeError(f"{label} must be a JSON argv array: {error}") from error
    if not isinstance(parsed, list) or not parsed or any(not isinstance(item, str) or not item for item in parsed):
        raise argparse.ArgumentTypeError(f"{label} must be a non-empty JSON argv array of strings")
    return parsed


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--mission", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--console-client", type=Path, required=True)
    parser.add_argument("--mission-preflight-command-json", type=lambda value: json_argv(value, "preflight command"), required=True)
    parser.add_argument(
        "--preferences-mode",
        choices=("isolated", "native"),
        default="isolated",
        help="use an empty isolated profile (default) or an explicit existing native FSO preferences directory",
    )
    parser.add_argument("--profile-dir", type=Path, help="empty isolated preferences directory; required in isolated mode")
    parser.add_argument("--preferences-dir", type=Path, help="existing native FSO preferences directory; required in native mode")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--duration-seconds", type=float, required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=42042)
    parser.add_argument("--stale-ms", type=int, default=3000)
    parser.add_argument("--workdir", type=Path)
    parser.add_argument("--game-args-json", type=lambda value: json_argv(value, "game arguments"), default=[])
    parser.add_argument(
        "--launch-mode",
        choices=("mainhall", "start-mission"),
        default="mainhall",
        help="launch into the main hall (default) or explicitly start --mission",
    )
    parser.add_argument(
        "--mode",
        choices=("normal", "restart-mission", "restart-process"),
        default="normal",
        help=("normal: one uninterrupted observation; restart-mission: external manual checkpoint "
              "(Restart Mission, then Fly Mission); restart-process: runner-controlled relaunch"),
    )
    parser.add_argument(
        "--restart-command-json",
        type=lambda value: json_argv(value, "restart command"),
        help=("restart-mission only: external manual-checkpoint command. It must return only after "
              "the operator selected Restart Mission, reached the briefing, selected Fly Mission, "
              "and the mission is back in the cockpit; it never automates game UI."),
    )
    return parser.parse_args(argv)


def require_inputs(args: argparse.Namespace) -> tuple[str | None, dict[str, Any] | None]:
    for label, path in (("binary", args.binary), ("mission", args.mission), ("config", args.config), ("console client", args.console_client)):
        if not path.is_file(): return f"missing {label}: {path}", None
    if args.mission.suffix.lower() != ".fs2": return "mission must be a .fs2 file", None
    if args.duration_seconds <= 0 or not 1 <= args.port <= 65535 or args.stale_ms < 1: return "duration, port or stale timeout is invalid", None
    try:
        config = json.loads(args.config.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return f"invalid telemetry config: {error}", None
    if not isinstance(config, dict) or config.get("enabled") is not True or config.get("bindPort") != args.port:
        return "config must enable telemetry and set bindPort equal to --port", None
    if args.mode == "restart-mission" and args.restart_command_json is None:
        return ("mode restart-mission requires --restart-command-json as an external manual checkpoint: "
                "Restart Mission, then Fly Mission before the hook returns"), None
    if args.mode == "restart-mission" and args.launch_mode != "start-mission":
        return "restart-mission requires --launch-mode start-mission", None
    if args.mode == "restart-process" and args.restart_command_json is not None:
        return "restart-process is runner-controlled; do not provide --restart-command-json", None
    if any(argument.casefold() == "-start_mission" for argument in args.game_args_json):
        return "game arguments must not contain runner-owned -start_mission; use --launch-mode start-mission", None
    if args.preferences_mode == "isolated":
        if args.profile_dir is None:
            return "isolated preferences mode requires --profile-dir", None
        if args.preferences_dir is not None:
            return "isolated preferences mode does not accept --preferences-dir", None
    else:
        if args.preferences_dir is None:
            return "native preferences mode requires --preferences-dir", None
        if args.profile_dir is not None:
            return "native preferences mode does not accept --profile-dir", None
        if not args.preferences_dir.is_dir():
            return f"native preferences directory must already exist: {args.preferences_dir}", None
    return None, config


def transcript_summary(stdout: str) -> dict[str, Any]:
    events: list[dict[str, Any]] = []
    for line in stdout.splitlines():
        try: candidate = json.loads(line)
        except json.JSONDecodeError: continue
        if isinstance(candidate, dict) and isinstance(candidate.get("status"), str): events.append(candidate)
    live = [item for item in events if item["status"] == "Live" and str(item.get("session", "0")) not in ("", "0", "none")]
    final = events[-1] if events else {}
    return {"eventCount": len(events), "liveCount": len(live), "finalStatus": final.get("status"),
            "session": str(final.get("session")) if final.get("session") is not None else None,
            "missionGeneration": final.get("mission_generation"),
            "chronologyLive": bool(live) and final.get("status") == "Live" and str(final.get("session", "0")) not in ("", "0", "none")}


def run_client(command: list[str], cwd: Path, env: dict[str, str], stdout_path: Path, stderr_path: Path) -> tuple[int, dict[str, Any]]:
    with stdout_path.open("w", encoding="utf-8", newline="\n") as out, stderr_path.open("w", encoding="utf-8", newline="\n") as err:
        completed = subprocess.run(command, cwd=cwd, env=env, stdout=out, stderr=err, text=True, check=False)
    return completed.returncode, transcript_summary(stdout_path.read_text(encoding="utf-8"))


def run_command(command: list[str], cwd: Path, env: dict[str, str], stdout_path: Path, stderr_path: Path) -> int:
    with stdout_path.open("w", encoding="utf-8", newline="\n") as out, stderr_path.open("w", encoding="utf-8", newline="\n") as err:
        return subprocess.run(command, cwd=cwd, env=env, stdout=out, stderr=err, text=True, check=False).returncode


def require_live(label: str, evidence: dict[str, Any]) -> None:
    if not evidence["chronologyLive"]:
        raise RuntimeError(f"{label} console chronology did not end in a Live session")


def require_profile_has_only_telemetry_config(profile_dir: Path, config_path: Path) -> None:
    allowed = config_path.resolve()
    unexpected = [path for path in profile_dir.rglob("*") if path.is_file() and path.resolve() != allowed]
    if unexpected:
        raise RuntimeError(f"isolated preferences profile contains unexpected files: {unexpected[0]}")


class DeploymentGuard:
    def __init__(self, source: Path, target: Path, backup: Path):
        self.source, self.target, self.backup = source, target, backup
        self.had_original = target.is_file(); self.original_sha256 = sha256(target) if self.had_original else None; self.deployed_sha256: str | None = None
    def deploy(self) -> None:
        self.target.parent.mkdir(parents=True, exist_ok=True)
        if self.target.exists() and not self.target.is_file(): raise RuntimeError(f"deployment target is not a file: {self.target}")
        if self.had_original: self.backup.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(self.target, self.backup)
        temporary = self.target.with_name(f".{self.target.name}.physical-soak.tmp"); shutil.copy2(self.source, temporary); os.replace(temporary, self.target); self.deployed_sha256 = sha256(self.target)
    def restore(self) -> None:
        if self.deployed_sha256 is None: return
        if not self.target.is_file() or sha256(self.target) != self.deployed_sha256: raise RuntimeError(f"refusing to overwrite changed deployment target: {self.target}")
        if self.had_original:
            temporary = self.target.with_name(f".{self.target.name}.restore.tmp"); shutil.copy2(self.backup, temporary); os.replace(temporary, self.target)
            if sha256(self.target) != self.original_sha256: raise RuntimeError(f"restoration hash mismatch: {self.target}")
        else: self.target.unlink()
    def evidence(self) -> dict[str, Any]:
        return {"source": str(self.source.resolve()), "target": str(self.target.resolve()), "sourceSha256": sha256(self.source), "hadOriginal": self.had_original, "originalSha256": self.original_sha256, "deployedSha256": self.deployed_sha256}


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    input_error, _ = require_inputs(args)
    if input_error: print(input_error, file=sys.stderr); return 2
    workdir = args.workdir.resolve() if args.workdir else args.binary.resolve().parent
    if not workdir.is_dir(): print(f"missing workdir: {workdir}", file=sys.stderr); return 2
    if args.binary.resolve().parent != workdir:
        print(f"binary must reside directly in --workdir because the engine changes to its executable directory: {args.binary.resolve().parent} != {workdir}", file=sys.stderr)
        return 2
    if args.preferences_mode == "isolated":
        assert args.profile_dir is not None
        if args.profile_dir.exists() and any(args.profile_dir.iterdir()): print(f"profile directory must be empty: {args.profile_dir}", file=sys.stderr); return 2
        args.profile_dir.mkdir(parents=True)
        preferences_dir = args.profile_dir.resolve()
        profile_config = preferences_dir / "data" / "config" / "telemetry.json"
    else:
        assert args.preferences_dir is not None
        preferences_dir = args.preferences_dir.resolve()
        profile_config = preferences_dir / "data" / "config" / "telemetry.json"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    game_mission = workdir / "data" / "missions" / args.mission.name
    config_guard = DeploymentGuard(args.config, profile_config, args.output_dir / "backup" / "telemetry.json")
    mission_guard = DeploymentGuard(args.mission, game_mission, args.output_dir / "backup" / args.mission.name) if args.launch_mode == "start-mission" else None
    env = os.environ.copy()
    if args.preferences_mode == "isolated":
        # Windows CFile obtains its user profile from the platform APIs, not HOME.
        # Keep that profile intact while FSO_PREFERENCES_PATH selects the temporary
        # profile containing the guarded telemetry configuration for this campaign.
        if os.name != "nt": env["HOME"] = str(preferences_dir)
        env["FSO_PREFERENCES_PATH"] = str(preferences_dir)
    else:
        # Native mode deliberately leaves the user's profile and preference lookup
        # untouched.  The config is guarded at the explicit native location only.
        env.pop("FSO_PREFERENCES_PATH", None)
    env.update({"FSO_TELEMETRY_PHYSICAL_SOAK_MISSION": str(game_mission.resolve()), "FSO_TELEMETRY_PHYSICAL_SOAK_CONFIG": str(profile_config.resolve())})
    paths = {name: args.output_dir / f"{name}.log" for name in ("preflight.stdout", "preflight.stderr", "producer.stdout", "producer.stderr", "pre.console.stdout", "pre.console.stderr", "post.console.stdout", "post.console.stderr", "restart.stdout", "restart.stderr")}
    report_path = args.output_dir / "physical-soak-report.json"
    # Game arguments are an explicit operator contract.  Do not append display
    # or focus flags here: those flags can change startup behaviour and must be
    # supplied by --game-args-json when wanted.  start-mission is the sole
    # runner-owned opt-in addition.
    game_command = [str(args.binary.resolve()), *args.game_args_json]
    if args.launch_mode == "start-mission":
        game_command.extend(("-start_mission", args.mission.name))
    attestation = ({"classification": "diagnostic-only", "ac16Eligible": False, "reason": "launchMode=mainhall does not attest a mission run"}
                   if args.launch_mode == "mainhall" else
                   {"classification": "ac16-eligible-candidate", "ac16Eligible": True, "reason": "launchMode=start-mission is eligible for AC16 review subject to all other evidence"})
    report: dict[str, Any] = {"schema": SCHEMA, "status": "running", "startedAtUtc": datetime.now(timezone.utc).isoformat(), "mode": args.mode, "launchMode": args.launch_mode, "attestation": attestation,
        "inputs": {**{key: {"path": str(path.resolve()), "sha256": sha256(path)} for key, path in (("binary", args.binary), ("mission", args.mission), ("config", args.config), ("consoleClient", args.console_client))},
                   "workdir": {"path": str(workdir)}},
        "profile": {"mode": args.preferences_mode, "path": str(preferences_dir), "telemetryConfig": str(profile_config.resolve()), "sourceConfigSha256": sha256(args.config),
                    "environmentPolicy": ("preserve-windows-user-profile" if os.name == "nt" else "isolated-home") if args.preferences_mode == "isolated" else "preserve-native-environment",
                    "configurationPolicy": "isolated-fso-preferences-path" if args.preferences_mode == "isolated" else "native-guarded-telemetry-config",
                    "preferencesEnvironmentVariable": "FSO_PREFERENCES_PATH" if args.preferences_mode == "isolated" else None}, "deployment": {},
        "preflight": {"command": args.mission_preflight_command_json, "exitCode": None}, "bindHandshake": {"bindPort": args.port, "observed": False},
        "producer": {"command": game_command, "initialPid": None, "initialExitCode": None, "restartPid": None, "restartExitCode": None}, "phases": {},
        "restart": {
            "mode": "not-requested" if args.mode == "normal" else args.mode,
            "control": "none" if args.mode == "normal" else ("external-manual-checkpoint" if args.mode == "restart-mission" else "runner-controlled"),
            "command": args.restart_command_json,
            "exitCode": None,
            "checkpoint": (
                {"operatorAction": "Restart Mission, wait for the briefing, then select Fly Mission", 
                 "completionSignal": "external command exits 0 only after the cockpit is active again",
                 "postObservation": "must end Live with a new session and increased missionGeneration"}
                if args.mode == "restart-mission" else None),
        }}
    producer: subprocess.Popen[str] | None = None
    try:
        config_guard.deploy()
        if args.preferences_mode == "isolated":
            require_profile_has_only_telemetry_config(preferences_dir, profile_config)
        report["deployment"] = {"config": config_guard.evidence(), "mission": (mission_guard.evidence() if mission_guard else {"deployed": False, "reason": "launchMode=mainhall"})}
        if mission_guard is not None:
            mission_guard.deploy(); report["deployment"]["mission"] = mission_guard.evidence()
        preflight_rc = run_command(args.mission_preflight_command_json, workdir, env, paths["preflight.stdout"], paths["preflight.stderr"]); report["preflight"]["exitCode"] = preflight_rc
        if preflight_rc != 0: raise RuntimeError(f"mission preflight failed with {preflight_rc}")
        producer = subprocess.Popen(game_command, cwd=workdir, env=env, stdout=paths["producer.stdout"].open("w", encoding="utf-8", newline="\n"), stderr=paths["producer.stderr"].open("w", encoding="utf-8", newline="\n"), text=True)
        report["producer"]["initialPid"] = producer.pid
        env["FSO_TELEMETRY_PHYSICAL_SOAK_INITIAL_PID"] = str(producer.pid)
        started = time.monotonic(); pre_seconds = args.duration_seconds if args.mode == "normal" else args.duration_seconds / 2
        pre_cmd = [sys.executable, str(args.console_client.resolve()), "--host", args.host, "--port", str(args.port), "--seconds", str(pre_seconds), "--stale-ms", str(args.stale_ms)]
        pre_rc, pre = run_client(pre_cmd, workdir, env, paths["pre.console.stdout"], paths["pre.console.stderr"]); report["phases"]["pre"] = {"command": pre_cmd, "exitCode": pre_rc, **pre}
        if producer.poll() is not None: raise RuntimeError(f"producer exited before or during pre-client with {producer.returncode}")
        if pre_rc != 0: raise RuntimeError(f"pre console client failed with {pre_rc}")
        require_live("pre", pre); report["bindHandshake"]["observed"] = True
        if args.mode != "normal":
            if args.mode == "restart-mission":
                restart_rc = run_command(args.restart_command_json, workdir, env, paths["restart.stdout"], paths["restart.stderr"]); report["restart"]["exitCode"] = restart_rc
                if restart_rc != 0: raise RuntimeError(f"external restart hook failed with {restart_rc}")
                if producer.poll() is not None: raise RuntimeError(f"mission restart hook ended producer with {producer.returncode}")
            else:
                producer.terminate()
                try: producer.wait(timeout=10)
                except subprocess.TimeoutExpired: producer.kill(); producer.wait(timeout=10)
                report["producer"]["initialExitCode"] = producer.returncode
                env["FSO_TELEMETRY_PHYSICAL_SOAK_PROCESS_EPOCH"] = "2"
                producer = subprocess.Popen(game_command, cwd=workdir, env=env, stdout=paths["restart.stdout"].open("w", encoding="utf-8", newline="\n"), stderr=paths["restart.stderr"].open("w", encoding="utf-8", newline="\n"), text=True)
                report["producer"]["restartPid"] = producer.pid
                if producer.pid == report["producer"]["initialPid"]: raise RuntimeError("runner restart did not create a new producer PID")
            post_cmd = [sys.executable, str(args.console_client.resolve()), "--host", args.host, "--port", str(args.port), "--seconds", str(args.duration_seconds - pre_seconds), "--stale-ms", str(args.stale_ms)]
            post_rc, post = run_client(post_cmd, workdir, env, paths["post.console.stdout"], paths["post.console.stderr"]); report["phases"]["post"] = {"command": post_cmd, "exitCode": post_rc, **post}
            if args.mode == "restart-mission" and producer.poll() is not None: raise RuntimeError(f"producer exited during post-client after mission restart with {producer.returncode}")
            if args.mode == "restart-process" and producer.poll() is not None: raise RuntimeError(f"restarted producer exited during post-client with {producer.returncode}")
            if post_rc != 0: raise RuntimeError(f"post console client failed with {post_rc}")
            require_live("post after Restart Mission then Fly Mission" if args.mode == "restart-mission" else "post", post)
            if args.mode == "restart-mission" and (post["session"] == pre["session"] or post["missionGeneration"] is None or pre["missionGeneration"] is None or post["missionGeneration"] <= pre["missionGeneration"]):
                raise RuntimeError("mission restart checkpoint did not reach Fly Mission: post observation must be a new Live session with increased mission generation")
            if args.mode == "restart-process" and post["session"] == pre["session"]: raise RuntimeError("process restart was not observed as a new session")
        elapsed = time.monotonic() - started; report["wallDurationSeconds"] = elapsed
        if elapsed < args.duration_seconds: raise RuntimeError(f"wall duration {elapsed:.3f}s is shorter than requested {args.duration_seconds:.3f}s")
        report["status"] = "passed"
    except (OSError, RuntimeError, ValueError) as error:
        report["status"] = "failed"; report["error"] = str(error)
    finally:
        if producer is not None:
            if report["producer"]["restartPid"] is None: report["producer"]["initialExitCode"] = producer.poll()
            if producer.poll() is None:
                producer.terminate()
                try: producer.wait(timeout=10)
                except subprocess.TimeoutExpired: producer.kill(); producer.wait(timeout=10)
            if report["producer"]["restartPid"] is None: report["producer"]["initialExitCode"] = producer.returncode
            else: report["producer"]["restartExitCode"] = producer.returncode
        report["completedAtUtc"] = datetime.now(timezone.utc).isoformat()
        for name, path in paths.items():
            if path.exists(): report.setdefault("artifacts", {})[name] = {"path": str(path.resolve()), "sha256": sha256(path)}
        restoration: dict[str, dict[str, Any]] = {}
        for name, guard in (("mission", mission_guard), ("config", config_guard)):
            if guard is None:
                restoration[name] = {"attempted": False, "restored": False, "reason": "launchMode=mainhall"}
                continue
            try:
                guard.restore(); restoration[name] = {"attempted": True, "restored": True}
            except RuntimeError as error:
                restoration[name] = {"attempted": True, "restored": False, "restoreError": str(error)}
                report["status"] = "failed"; report["error"] = str(error)
        report["deployment"]["restoration"] = restoration
        write_json(report_path, report)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__": raise SystemExit(main())
