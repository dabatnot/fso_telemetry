#!/usr/bin/env python3
"""Run and fail-closed validate Phase 2 Release performance and soak evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Sequence


SCHEMA = "fs2open.telemetry.phase2.performance-campaign.v1"
HARNESS_SCHEMA = "fs2open.telemetry.phase2.performance-harness.v1"
SOAKS = (
    "continuous-all-blocks",
    "mission-reentry-respawn",
    "client-stop-restart",
    "p2-loss-gate-resync",
    "four-clients-one-slow",
)
BUDGETS = {
    "sharedBytes": 67_108_864,
    "perClientBytes": 83_886_080,
    "processFourClientsBytes": 402_653_184,
}


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--warmup-seconds", type=int, default=60)
    parser.add_argument("--measure-seconds", type=int, default=1800)
    parser.add_argument("--minimum-frames", type=int, default=100000)
    parser.add_argument("--minimum-flight-ticks", type=int, default=54000)
    parser.add_argument("--minimum-system-ticks", type=int, default=18000)
    parser.add_argument("--minimum-keyframes", type=int, default=900)
    parser.add_argument("--seed", type=int, default=4242)
    parser.add_argument("--soak-seconds", type=int, default=1800)
    parser.add_argument("--soak-seed", type=int, default=4242)
    parser.add_argument("--performance-report-dir", type=Path, required=True)
    parser.add_argument("--soak-report-dir", type=Path, required=True)
    parser.add_argument("--harness", type=Path)
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
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_harness(args: argparse.Namespace) -> Path:
    if args.harness:
        return args.harness
    names = ("telemetry_phase2_performance_harness.exe", "telemetry_phase2_performance_harness")
    for root in (args.build_dir / "bin" / args.config, args.build_dir / "bin", args.build_dir):
        for name in names:
            candidate = root / name
            if candidate.is_file():
                return candidate
    raise ValueError("telemetry_phase2_performance_harness executable not found")


def harness_environment(args: argparse.Namespace) -> dict[str, str]:
    environment = os.environ.copy()
    if os.name == "nt":
        runtime_dirs = [
            args.build_dir / "lib" / "prebuilt" / "openal" / "bin",
            args.build_dir / "lib" / "prebuilt" / "ffmpeg" / "bin",
            args.build_dir / "lib" / "prebuilt" / "freetype" / "lib",
            args.build_dir / "lib" / "prebuilt" / "sdl2" / "lib",
        ]
        existing = [str(path.resolve()) for path in runtime_dirs if path.is_dir()]
        environment["PATH"] = os.pathsep.join(existing + [environment.get("PATH", "")])
    return environment


def validate_report(report: Any, args: argparse.Namespace) -> None:
    require(isinstance(report, dict), "harness report must be an object")
    require(report.get("schema") == HARNESS_SCHEMA, "wrong performance schema")
    require(report.get("status") == "passed", "performance harness did not pass")
    require(report.get("certificationEligible") is True,
            "smoke/accelerated output cannot certify performance or soaks")
    protocol = report.get("measurementProtocol")
    require(isinstance(protocol, dict), "missing measurement protocol")
    expected_protocol = {
        "configuration": "Release",
        "warmupSeconds": args.warmup_seconds,
        "measuredSeconds": args.measure_seconds,
        "minimumFrames": args.minimum_frames,
        "minimumFlightTicks": args.minimum_flight_ticks,
        "minimumSystemTicks": args.minimum_system_ticks,
        "minimumKeyframes": args.minimum_keyframes,
        "percentile": "nearest-rank",
        "outliersRemoved": 0,
        "clock": "steady_clock",
    }
    for key, value in expected_protocol.items():
        require(protocol.get(key) == value, f"wrong measurementProtocol.{key}")
    required_wall = 9 * (args.warmup_seconds + args.measure_seconds) + \
        5 * (args.warmup_seconds + args.soak_seconds)
    require(protocol.get("requiredWallElapsedSeconds") == required_wall,
            "wrong required wall-clock campaign duration")
    require(isinstance(protocol.get("wallElapsedSeconds"), (int, float)) and
            protocol["wallElapsedSeconds"] >= required_wall,
            "campaign did not execute for the required wall-clock duration")
    for observed, minimum in (
        ("observedFrames", args.minimum_frames),
        ("observedFlightTicks", args.minimum_flight_ticks),
        ("observedSystemTicks", args.minimum_system_ticks),
        ("observedKeyframes", args.minimum_keyframes),
    ):
        require(isinstance(protocol.get(observed), int) and
                protocol[observed] >= minimum,
                f"measurement minimum not met: {observed}")
    host = report.get("host")
    require(isinstance(host, dict), "missing host/environment metadata")
    for key in ("compiler", "powerMode", "temperatureLog", "throttleLog"):
        require(isinstance(host.get(key), str) and host[key] and
                host[key] != "unavailable", f"missing host.{key}")

    raw = report.get("rawSamples")
    require(isinstance(raw, list) and len(raw) == 9,
            "nine distinct raw performance series are required")
    raw_names = {item.get("workload") for item in raw if isinstance(item, dict)}
    require(raw_names == {"noModule", "configAbsent", "disabledBaseline",
                          "flightControl", "systemsNominal", "maximumBounds",
                          "fourClientsSystems", "wouldBlockLossResync",
                          "manifestChanged"},
            "wrong raw performance workload inventory")
    for item in raw:
        require(isinstance(item.get("path"), str) and item["path"],
                "missing raw sample path")
        path = Path(item["path"])
        require(path.is_file(), f"raw sample file is missing: {path}")
        require(item.get("sha256") == sha256(path),
                f"raw sample hash mismatch: {path}")
        require(isinstance(item.get("count"), int) and
                item["count"] >= args.minimum_frames,
                f"insufficient raw samples: {path}")

    timings = report.get("timingsMs")
    require(isinstance(timings, dict), "missing timing proof")
    limits = {
        ("flightControl", "p99"): 0.25,
        ("systemsNominal", "p99"): 0.75,
        ("keyframe", "p99"): 2.0,
        ("keyframe", "max"): 5.0,
        ("fourClientsSystems", "p99"): 1.50,
    }
    for (workload, statistic), limit in limits.items():
        sample = timings.get(workload)
        require(isinstance(sample, dict), f"missing timing workload: {workload}")
        value = sample.get(statistic)
        require(isinstance(value, (int, float)) and value <= limit,
                f"{workload}.{statistic} exceeds {limit} ms")
    regression = timings.get("activeControlMedianRegressionPercent")
    require(isinstance(regression, (int, float)) and regression < 2.0,
            "active/control median regression is not below 2%")

    allocations = report.get("allocations")
    require(isinstance(allocations, dict), "missing allocation proof")
    require(allocations.get("afterReady") == 0, "allocation occurred after Ready")
    require(allocations.get("steadyStateGrowthBytes") == 0, "steady-state memory grew")
    budget = report.get("budgets")
    require(isinstance(budget, dict), "missing budget-boundary proof")
    for key, limit in BUDGETS.items():
        proof = budget.get(key)
        require(isinstance(proof, dict), f"missing budget: {key}")
        require(proof.get("limit") == limit, f"wrong {key} limit")
        require(proof.get("exactAccepted") is True, f"{key} exact limit was not accepted")
        require(proof.get("plusOneRejectedBeforeBind") is True,
                f"{key} +1 was not rejected before bind")

    soaks = report.get("soaks")
    require(isinstance(soaks, list) and len(soaks) == len(SOAKS),
            "five soak reports are required")
    by_name = {item.get("name"): item for item in soaks if isinstance(item, dict)}
    require(set(by_name) == set(SOAKS), "wrong canonical soak inventory")
    derived = set()
    for name in SOAKS:
        soak = by_name[name]
        require(soak.get("baseSeed") == args.soak_seed, f"wrong soak seed: {name}")
        require(soak.get("seedDerivation") == "(4242,canonical-scenario-name)",
                f"wrong soak seed derivation: {name}")
        require(isinstance(soak.get("derivedStream"), str) and soak["derivedStream"],
                f"missing derived soak stream: {name}")
        require(soak["derivedStream"] not in derived, f"duplicate soak stream: {name}")
        derived.add(soak["derivedStream"])
        require(soak.get("durationSeconds") == args.soak_seconds,
                f"wrong soak duration: {name}")
        require(isinstance(soak.get("workload"), str) and soak["workload"],
                f"missing native soak workload: {name}")
        require(isinstance(soak.get("observedEvents"), int) and
                soak["observedEvents"] > 0,
                f"soak has no observed event: {name}")
        state_hash = soak.get("stateHash")
        require(isinstance(state_hash, dict), f"missing state hash proof: {name}")
        require(isinstance(state_hash.get("oracle"), str) and
                state_hash["oracle"] not in ("", "0"),
                f"missing oracle state hash: {name}")
        require(isinstance(state_hash.get("final"), str) and
                state_hash["final"] not in ("", "0"),
                f"missing final state hash: {name}")
        require(state_hash.get("equal") is True and
                state_hash["oracle"] == state_hash["final"],
                f"final state differs from workload oracle: {name}")
        if name == "continuous-all-blocks":
            coverage = soak.get("blockCoverage")
            require(isinstance(coverage, dict),
                    "missing continuous workload block coverage")
            require(coverage.get("count") == 11 and
                    coverage.get("complete") is True,
                    "continuous workload did not cover all 11 contract blocks")
        if name in ("mission-reentry-respawn", "client-stop-restart"):
            require(soak.get("observedEvents") == 3,
                    f"{name} must execute exactly three cycles")
            require(soak.get("cycles") == 3,
                    f"{name} cycle counter must equal three")
        if name == "mission-reentry-respawn":
            require(soak.get("respawns") == 3,
                    "mission-reentry-respawn must execute exactly three intra-session respawns")
            require(soak.get("reentries") == 3,
                    "mission-reentry-respawn must execute exactly three mission reentries")
        if name == "client-stop-restart":
            require(soak.get("restarts") == 3,
                    "client-stop-restart must execute exactly three client restarts")
        require(isinstance(soak.get("rawSha256"), str) and
                len(soak["rawSha256"]) == 64,
                f"missing soak raw evidence: {name}")
        for key in ("noLeak", "noGrowth", "noDeadlock", "purgedToZero",
                    "idsNotReused", "finalStateExact", "shutdownBounded"):
            require(soak.get(key) is True, f"failed soak oracle {name}.{key}")


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        require(args.config.lower() == "release", "performance evidence requires Release")
        require(args.warmup_seconds == 60, "warm-up must be 60 seconds")
        require(args.measure_seconds >= 1800, "measurement must be at least 1800 seconds")
        require(args.minimum_frames >= 100000, "minimum frames must be at least 100000")
        require(args.minimum_flight_ticks >= 54000, "minimum flight ticks must be at least 54000")
        require(args.minimum_system_ticks >= 18000, "minimum systems ticks must be at least 18000")
        require(args.minimum_keyframes >= 900, "minimum keyframes must be at least 900")
        require(args.seed == 4242 and args.soak_seed == 4242, "normative seed must be 4242")
        require(args.soak_seconds == 1800, "each soak must be 1800 seconds")
        harness = find_harness(args)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2

    report_path = args.performance_report_dir / "harness-report.json"
    command = [
        str(harness), "--warmup-seconds", str(args.warmup_seconds),
        "--measure-seconds", str(args.measure_seconds),
        "--minimum-frames", str(args.minimum_frames),
        "--minimum-flight-ticks", str(args.minimum_flight_ticks),
        "--minimum-system-ticks", str(args.minimum_system_ticks),
        "--minimum-keyframes", str(args.minimum_keyframes),
        "--seed", str(args.seed), "--soak-seconds", str(args.soak_seconds),
        "--soak-seed", str(args.soak_seed),
        "--soak-report-dir", str(args.soak_report_dir),
        "--report", str(report_path),
    ]
    args.performance_report_dir.mkdir(parents=True, exist_ok=True)
    args.soak_report_dir.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(command, text=True, capture_output=True, check=False,
                               env=harness_environment(args))
    (args.performance_report_dir / "stdout.log").write_text(completed.stdout, encoding="utf-8")
    (args.performance_report_dir / "stderr.log").write_text(completed.stderr, encoding="utf-8")
    try:
        require(completed.returncode == 0, f"harness exit code {completed.returncode}")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        validate_report(report, args)
        campaign = {
            "schema": SCHEMA,
            "status": "passed",
            "command": command,
            "harness": str(harness.resolve()),
            "harnessSha256": sha256(harness),
            "reportSha256": sha256(report_path),
        }
        write_json(args.performance_report_dir / "campaign-manifest.json", campaign)
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        write_json(args.performance_report_dir / "campaign-manifest.json", {
            "schema": SCHEMA, "status": "failed", "command": command, "error": str(error)
        })
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
