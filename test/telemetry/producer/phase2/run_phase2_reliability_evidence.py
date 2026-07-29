#!/usr/bin/env python3
"""Run and fail-closed validate the normative Phase 2 P2-LOSS-GATE matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


SCHEMA = "fs2open.telemetry.phase2.loss-campaign.v1"
RUN_SCHEMA = "fs2open.telemetry.phase2.loss-run.v1"
PROFILES = ("core-gate", "complete-ship")
PROFILE_CODES = {"core-gate": 0x0401, "complete-ship": 0x0583}
LOSS_RATES = (1, 5, 20)
MODES = ("iid", "burst")
SEED = 1345474380
CORE_MUTATIONS = frozenset(
    ("identity", "lifecycle", "hull", "shields", "energy", "propulsion")
)
COMPLETE_MUTATIONS = frozenset(
    (
        "identity",
        "lifecycle",
        "hull",
        "shields",
        "energy",
        "propulsion",
        "control",
        "bank",
        "support",
        "topology",
        "respawn",
        "manifest",
    )
)
CONVERGENCE_ORACLES = (
    "sourceClosureStable",
    "manifestAppliedBeforeClock",
    "allAtomsEqualOracle",
    "noForbiddenAtom",
    "noPartialPublication",
    "liveAfterReplacementSnapshot",
    "boundedResources",
)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profiles", nargs="+", choices=PROFILES, default=list(PROFILES))
    parser.add_argument("--loss-rates", nargs="+", type=int, choices=LOSS_RATES, default=list(LOSS_RATES))
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    parser.add_argument("--seed", type=int, default=SEED)
    parser.add_argument("--warmup-seconds", type=int, default=60)
    parser.add_argument("--duration-seconds", type=int, default=600)
    parser.add_argument("--keyframe-seconds", type=float, default=2.0)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--report-dir", type=Path, required=True)
    parser.add_argument("--harness", type=Path)
    parser.add_argument("--resume", action="store_true")
    return parser.parse_args(argv)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def find_harness(args: argparse.Namespace) -> Path:
    if args.harness:
        return args.harness
    names = ("telemetry_phase2_loss_harness.exe", "telemetry_phase2_loss_harness")
    roots = (args.build_dir / "bin" / args.config, args.build_dir / "bin", args.build_dir)
    for root in roots:
        for name in names:
            candidate = root / name
            if candidate.is_file():
                return candidate
    raise ValueError("telemetry_phase2_loss_harness executable not found")


def harness_environment(build_dir: Path) -> tuple[dict[str, str], list[str]]:
    environment = os.environ.copy()
    runtime_paths: list[str] = []
    if os.name == "nt":
        for relative in (
            Path("lib/prebuilt/openal/bin"),
            Path("lib/prebuilt/ffmpeg/bin"),
            Path("lib/prebuilt/freetype/lib"),
            Path("lib/prebuilt/sdl2/lib"),
        ):
            candidate = (build_dir / relative).resolve()
            if candidate.is_dir():
                runtime_paths.append(str(candidate))
        environment["PATH"] = os.pathsep.join(
            [*runtime_paths, environment.get("PATH", "")]
        )
    return environment, runtime_paths


def validate_run(report: Any, *, profile: str, rate: int, mode: str,
                 seed: int, warmup: int, duration: int, bound_us: int) -> None:
    require(isinstance(report, dict), "run report must be an object")
    require(report.get("schema") == RUN_SCHEMA, "wrong loss-run schema")
    require(report.get("status") == "passed", "loss harness did not pass")
    require(report.get("certificationEligible") is True,
            "accelerated smoke output cannot certify P2-LOSS-GATE")
    requested = report.get("requested")
    require(isinstance(requested, dict), "missing requested parameters")
    expected = {
        "profile": profile,
        "profileCode": PROFILE_CODES[profile],
        "lossRatePercent": rate,
        "mode": mode,
        "seed": seed,
        "warmupSeconds": warmup,
        "measuredSeconds": duration,
    }
    for key, value in expected.items():
        require(requested.get(key) == value, f"wrong requested.{key}")

    impairment = report.get("impairment")
    require(isinstance(impairment, dict), "missing impairment proof")
    require(impairment.get("independentDirectionalStreams") is True,
            "directional PRNG streams are not independent")
    require(impairment.get("streamKey") == "(profile,rate,mode,direction)",
            "wrong PRNG stream key")
    require(impairment.get("iidDistribution") == "bernoulli",
            "iid loss is not Bernoulli")
    require(impairment.get("burstBlockDatagrams") == 100,
            "burst block is not 100 datagrams")
    require(impairment.get("burstContiguousLength") == rate,
            "wrong contiguous burst length")
    require(impairment.get("duplicationPercent") == 2,
            "wrong duplication percentage")
    require(impairment.get("jitterUniformMs") == [0, 100],
            "wrong jitter interval")
    require(impairment.get("reorderWindowDatagrams") == 8,
            "wrong reorder window")
    require(impairment.get("blackouts") == [
        {"atMeasuredSecond": 120, "durationMs": 500},
        {"atMeasuredSecond": 360, "durationMs": 500},
    ], "wrong blackout schedule")
    decisions = impairment.get("decisionLog")
    require(isinstance(decisions, dict), "missing replayable decision log")
    require(isinstance(decisions.get("path"), str) and decisions["path"],
            "missing decision-log path")
    require(isinstance(decisions.get("sha256"), str) and len(decisions["sha256"]) == 64,
            "missing decision-log hash")
    require(decisions.get("schema") == "fs2open.telemetry.phase2.loss-decision.v1",
            "wrong decision-record schema")
    require(isinstance(decisions.get("records"), int) and decisions["records"] > 0,
            "decision log is empty")

    mutations = report.get("mutations")
    require(isinstance(mutations, dict), "missing mutation coverage")
    required = COMPLETE_MUTATIONS if profile == "complete-ship" else CORE_MUTATIONS
    require(required <= set(mutations.get("changedBlocks", [])),
            "not every required profile block was mutated")

    convergence = report.get("convergence")
    require(isinstance(convergence, dict), "missing convergence proof")
    require(convergence.get("clockOrigin") == "last-required-manifest-applied",
            "convergence clock has the wrong origin")
    require(isinstance(convergence.get("maximumUs"), int)
            and 0 < convergence["maximumUs"] <= bound_us,
            "convergence exceeded 2*keyframeSeconds+1s")
    required_times = ("impairmentEndUs", "sourceStableUs", "closureStableUs",
                      "manifestAppliedUs", "t0Us", "convergedUs")
    require(all(isinstance(convergence.get(key), int) for key in required_times),
            "missing observed convergence timestamps")
    require(isinstance(convergence.get("latestManifestId"), int)
            and convergence["latestManifestId"] > 0,
            "missing latest required manifest identity")
    require(isinstance(convergence.get("sessionAtT0"), int)
            and convergence["sessionAtT0"] > 0,
            "missing session identity at t0")
    require(isinstance(convergence.get("peerSessionId"), int)
            and convergence["peerSessionId"] == convergence["sessionAtT0"],
            "convergence did not use the same peer session as t0")
    require(convergence["t0Us"] == max(
        convergence["impairmentEndUs"], convergence["sourceStableUs"],
        convergence["closureStableUs"], convergence["manifestAppliedUs"]),
        "t0 is not max(impairment, source, closure, manifest)")
    require(convergence["maximumUs"] ==
            convergence["convergedUs"] - convergence["t0Us"],
            "maximumUs is not derived from observed timestamps")
    require(convergence.get("oracleStateHash") ==
            convergence.get("clientStateHash"),
            "final client state does not equal the oracle")
    conditions = convergence.get("conditions")
    require(isinstance(conditions, dict), "missing convergence conditions")
    for oracle in CONVERGENCE_ORACLES:
        require(conditions.get(oracle) is True, f"failed convergence oracle: {oracle}")


def validate_decision_log(path: Path, *, profile: str, rate: int,
                          mode: str, duration: int) -> None:
    counts = {"producer-to-client": 0, "client-to-producer": 0}
    streams: dict[str, str] = {}
    with path.open("r", encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            try:
                record = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"invalid decision JSONL line {line_number}: {error}") from error
            require(record.get("schema") ==
                    "fs2open.telemetry.phase2.loss-decision.v1",
                    f"wrong decision schema at line {line_number}")
            require(record.get("profile") == profile and
                    record.get("rate") == rate and record.get("mode") == mode,
                    f"decision tuple mismatch at line {line_number}")
            direction = record.get("direction")
            require(direction in counts, f"wrong direction at line {line_number}")
            require(record.get("ordinal") == counts[direction],
                    f"non-contiguous ordinal at line {line_number}")
            counts[direction] += 1
            key = record.get("streamKey")
            require(isinstance(key, str) and key.endswith("|" + direction),
                    f"wrong directional stream key at line {line_number}")
            streams[direction] = key
            for field in ("algorithmDrop", "blackout", "drop", "duplicate"):
                require(isinstance(record.get(field), bool),
                        f"missing {field} at line {line_number}")
            require(record["drop"] ==
                    (record["algorithmDrop"] or record["blackout"]),
                    f"drop composition mismatch at line {line_number}")
            require(0 <= record.get("jitterMs", -1) <= 100,
                    f"jitter outside [0,100] at line {line_number}")
            require(0 <= record.get("reorderSlot", -1) <= 8,
                    f"reorder slot outside [0,8] at line {line_number}")
            position = record.get("position")
            start = record.get("burstStart")
            require(isinstance(position, int) and 0 <= position < 100 and
                    isinstance(start, int) and 0 <= start < 100,
                    f"invalid burst coordinates at line {line_number}")
            if mode == "burst":
                require(record["algorithmDrop"] ==
                        ((position + 100 - start) % 100 < rate),
                        f"non-contiguous burst decision at line {line_number}")
    require(all(value > 0 for value in counts.values()),
            "decision log does not contain both directional streams")
    require(len(set(streams.values())) == 2,
            "directional streams are not independent")


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        require(args.seed == SEED, f"P2-LOSS-GATE seed must be {SEED}")
        require(args.warmup_seconds == 60, "P2-LOSS-GATE warm-up must be 60 seconds")
        require(args.duration_seconds == 600, "P2-LOSS-GATE measurement must be 600 seconds")
        require(len(set(args.profiles)) == len(args.profiles), "duplicate profile")
        require(len(set(args.loss_rates)) == len(args.loss_rates), "duplicate loss rate")
        require(len(set(args.modes)) == len(args.modes), "duplicate mode")
        harness = find_harness(args)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2

    manifest_path = args.report_dir / "campaign-manifest.json"
    if manifest_path.exists() and not args.resume:
        print(f"campaign already exists: {manifest_path}", file=sys.stderr)
        return 2
    campaign: dict[str, Any] = {
        "schema": SCHEMA,
        "status": "running",
        "startedAtUtc": datetime.now(timezone.utc).isoformat(),
        "harness": str(harness.resolve()),
        "harnessSha256": sha256(harness),
        "runs": [],
    }
    environment, runtime_paths = harness_environment(args.build_dir)
    campaign["runtimeSearchPath"] = runtime_paths
    write_json(manifest_path, campaign)
    bound_us = int((2.0 * args.keyframe_seconds + 1.0) * 1_000_000)
    all_passed = True
    for profile in args.profiles:
        for rate in args.loss_rates:
            for mode in args.modes:
                name = f"{profile}-{rate}-{mode}"
                run_dir = args.report_dir / name
                report_path = run_dir / "harness-report.json"
                decision_path = run_dir / "drop-decisions.jsonl"
                command = [
                    str(harness), "--profile", profile, "--loss-rate", str(rate),
                    "--mode", mode, "--seed", str(args.seed),
                    "--warmup-seconds", str(args.warmup_seconds),
                    "--duration-seconds", str(args.duration_seconds),
                    "--decision-log", str(decision_path), "--report", str(report_path),
                ]
                record: dict[str, Any] = {"name": name, "command": command, "status": "running"}
                campaign["runs"].append(record)
                write_json(manifest_path, campaign)
                try:
                    run_dir.mkdir(parents=True, exist_ok=True)
                    completed = subprocess.run(
                        command,
                        text=True,
                        capture_output=True,
                        check=False,
                        env=environment,
                    )
                    record["exitCode"] = completed.returncode
                    (run_dir / "stdout.log").write_text(completed.stdout, encoding="utf-8")
                    (run_dir / "stderr.log").write_text(completed.stderr, encoding="utf-8")
                    require(completed.returncode == 0, f"harness exit code {completed.returncode}")
                    report = json.loads(report_path.read_text(encoding="utf-8"))
                    validate_run(report, profile=profile, rate=rate, mode=mode,
                                 seed=args.seed, warmup=args.warmup_seconds,
                                 duration=args.duration_seconds, bound_us=bound_us)
                    require(decision_path.is_file(), "decision log was not produced")
                    require(report["impairment"]["decisionLog"]["sha256"] == sha256(decision_path),
                            "decision-log hash mismatch")
                    validate_decision_log(decision_path, profile=profile, rate=rate,
                                          mode=mode, duration=args.duration_seconds)
                    record.update(status="passed", reportSha256=sha256(report_path))
                except (OSError, ValueError, json.JSONDecodeError) as error:
                    record.update(status="failed", error=str(error))
                    all_passed = False
                write_json(manifest_path, campaign)
    campaign["status"] = "passed" if all_passed else "failed"
    campaign["completedAtUtc"] = datetime.now(timezone.utc).isoformat()
    write_json(manifest_path, campaign)
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
