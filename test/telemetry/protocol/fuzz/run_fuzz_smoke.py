#!/usr/bin/env python3
"""Run every telemetry fuzz target against its seeded corpus."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


TARGETS = (
    "fuzz_packet_reader",
    "fuzz_datagram",
    "fuzz_reassembler",
    "fuzz_control_payloads",
    "fuzz_records",
    "fuzz_transactions",
    "fuzz_comm_views",
    "fuzz_video_payloads",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=2000)
    parser.add_argument("--max-total-time", type=int)
    parser.add_argument("--max-len", type=int, default=2_097_220)
    parser.add_argument("--dictionary", type=Path)
    parser.add_argument("--standalone", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.runs < 0 or args.max_len <= 0 or (args.max_total_time is not None and args.max_total_time <= 0):
        print("runs must be non-negative and time/max-len must be positive", file=sys.stderr)
        return 2

    environment = os.environ.copy()
    environment.setdefault("ASAN_OPTIONS", "abort_on_error=1:detect_leaks=1:strict_string_checks=1")
    environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    suffix = ".exe" if os.name == "nt" else ""

    for target in TARGETS:
        executable = args.binary_dir / f"{target}{suffix}"
        corpus = args.corpus / target
        artifact_dir = args.artifacts / target
        artifact_dir.mkdir(parents=True, exist_ok=True)
        if not executable.is_file():
            print(f"missing fuzz executable: {executable}", file=sys.stderr)
            return 1
        if not corpus.is_dir():
            print(f"missing fuzz corpus: {corpus}", file=sys.stderr)
            return 1

        command = [str(executable), str(corpus)]
        if not args.standalone:
            command.extend(
                (
                    f"-max_len={args.max_len}",
                    "-timeout=10",
                    "-rss_limit_mb=2048",
                    "-print_final_stats=1",
                    "-use_value_profile=1",
                    f"-artifact_prefix={artifact_dir}{os.sep}",
                )
            )
            if args.max_total_time is None:
                command.append(f"-runs={args.runs}")
            else:
                command.append(f"-max_total_time={args.max_total_time}")
            if args.dictionary is not None:
                command.append(f"-dict={args.dictionary}")

        print(f"Running {target} ({'corpus replay' if args.standalone else 'libFuzzer'})", flush=True)
        result = subprocess.run(command, env=environment, check=False)
        if result.returncode != 0:
            return result.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
