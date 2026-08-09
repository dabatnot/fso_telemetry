#!/usr/bin/env python3
"""Run telemetry fuzz targets against their seeded corpora."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


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
PHASE2_TARGETS = (
    "telemetry_phase2_packet_reader_fuzz",
    "telemetry_phase2_state_validator_fuzz",
)
ALL_TARGETS = TARGETS + PHASE2_TARGETS
CORPUS_NAMES = {
    "telemetry_phase2_packet_reader_fuzz": "packet_reader",
    "telemetry_phase2_state_validator_fuzz": "state_validator",
}

EVIDENCE_SCHEMA = "FSTL-fuzz-evidence-v1"
CORPUS_MANIFEST_SCHEMA = "FSTL-fuzz-corpus-manifest-v1"
STAT_PATTERN = re.compile(r"^stat::([A-Za-z0-9_]+):\s*(.*?)\s*$")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=2000)
    parser.add_argument("--max-total-time", type=int)
    parser.add_argument("--max-len", type=int)
    parser.add_argument("--timeout", type=int, default=10)
    parser.add_argument("--rss-limit-mb", type=int, default=2048)
    parser.add_argument("--dictionary", type=Path)
    parser.add_argument("--standalone", action="store_true")
    parser.add_argument(
        "--target",
        action="append",
        choices=ALL_TARGETS,
        help="run only this target; repeat to select more than one (default: all eight targets)",
    )
    parser.add_argument("--seed", type=int, help="fixed libFuzzer seed recorded in campaign evidence")
    parser.add_argument(
        "--sanitizer",
        action="append",
        choices=("fuzzer", "address", "undefined"),
        default=[],
        help="sanitizer compiled into the target; repeat once per sanitizer",
    )
    parser.add_argument(
        "--evidence-dir",
        type=Path,
        help="archive logs, initial/final corpora, artifacts, hashes and a JSON report",
    )
    parser.add_argument(
        "--expected-sha",
        help="fail before running unless the checked-out Git revision exactly matches this candidate SHA",
    )
    return parser.parse_args(argv)


def selected_targets(args: argparse.Namespace) -> tuple[str, ...]:
    return tuple(args.target) if args.target else TARGETS


def paths_overlap(first: Path, second: Path) -> bool:
    first = first.resolve()
    second = second.resolve()
    return first == second or first in second.parents or second in first.parents


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tree_manifest(root: Path) -> dict[str, Any]:
    files: list[dict[str, Any]] = []
    total_bytes = 0
    if root.is_dir():
        for path in sorted(
            (candidate for candidate in root.rglob("*") if candidate.is_file()),
            key=lambda candidate: candidate.relative_to(root).as_posix(),
        ):
            relative_path = path.relative_to(root).as_posix()
            size = path.stat().st_size
            files.append({"path": relative_path, "size": size, "sha256": sha256_file(path)})
            total_bytes += size
    canonical_files = json.dumps(files, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return {
        "schema": CORPUS_MANIFEST_SCHEMA,
        "fileCount": len(files),
        "totalBytes": total_bytes,
        "treeSha256": hashlib.sha256(canonical_files).hexdigest(),
        "files": files,
    }


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def snapshot_tree(source: Path, destination: Path, manifest_path: Path) -> dict[str, Any]:
    if source.is_dir() and any(candidate.is_symlink() for candidate in source.rglob("*")):
        raise ValueError(f"refusing to archive a fuzz tree containing symbolic links: {source}")
    if destination.exists():
        shutil.rmtree(destination)
    if source.is_dir():
        shutil.copytree(source, destination)
    else:
        destination.mkdir(parents=True)
    manifest = tree_manifest(destination)
    write_json(manifest_path, manifest)
    return manifest


def command_version(command: Sequence[str]) -> dict[str, Any]:
    executable = shutil.which(command[0])
    if executable is None:
        return {"command": list(command), "available": False}
    try:
        result = subprocess.run(
            list(command), capture_output=True, check=False, text=True, errors="replace", timeout=30
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"command": list(command), "available": True, "path": executable, "error": str(error)}
    output = (result.stdout + result.stderr).strip()
    return {
        "command": list(command),
        "available": True,
        "path": executable,
        "exitCode": result.returncode,
        "version": output,
    }


def collect_tool_versions(environment: dict[str, str]) -> dict[str, Any]:
    compiler = environment.get("CXX", "clang++")
    return {
        "python": {
            "executable": sys.executable,
            "version": platform.python_version(),
        },
        "compiler": command_version((compiler, "--version")),
        "cmake": command_version(("cmake", "--version")),
        "ninja": command_version(("ninja", "--version")),
    }


def git_output(arguments: Sequence[str]) -> str | None:
    try:
        result = subprocess.run(
            ["git", *arguments], capture_output=True, check=False, text=True, errors="replace", timeout=30
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def revision_context(expected_sha: str | None) -> dict[str, Any]:
    git_head = git_output(("rev-parse", "HEAD"))
    git_status = git_output(("status", "--porcelain"))
    github_sha = os.environ.get("GITHUB_SHA")
    actual_sha = git_head or github_sha
    run_url = None
    if all(os.environ.get(name) for name in ("GITHUB_SERVER_URL", "GITHUB_REPOSITORY", "GITHUB_RUN_ID")):
        run_url = (
            f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}"
            f"/actions/runs/{os.environ['GITHUB_RUN_ID']}"
        )
    return {
        "expectedSha": expected_sha,
        "actualSha": actual_sha,
        "gitHead": git_head,
        "githubSha": github_sha,
        "matchesExpected": expected_sha is None or actual_sha == expected_sha,
        "workingTreeClean": git_status == "" if git_status is not None else None,
        "repository": os.environ.get("GITHUB_REPOSITORY"),
        "ref": os.environ.get("GITHUB_REF"),
        "runId": os.environ.get("GITHUB_RUN_ID"),
        "runAttempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
        "runUrl": run_url,
    }


def parse_final_stats(log_path: Path) -> dict[str, Any]:
    statistics: dict[str, Any] = {}
    if not log_path.is_file():
        return statistics
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = STAT_PATTERN.match(line.strip())
        if match is None:
            continue
        key, raw_value = match.groups()
        try:
            value: Any = int(raw_value)
        except ValueError:
            try:
                value = float(raw_value)
            except ValueError:
                value = raw_value
        statistics[key] = value
    return statistics


def tee_process(command: Sequence[str], environment: dict[str, str], log_path: Path) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8", newline="\n") as log:
        process = subprocess.Popen(
            list(command),
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            bufsize=1,
        )
        assert process.stdout is not None
        with process.stdout:
            for line in process.stdout:
                print(line, end="", flush=True)
                log.write(line)
                log.flush()
        return process.wait()


def build_command(
    args: argparse.Namespace, executable: Path, corpus: Path, artifact_dir: Path
) -> list[str]:
    command = [str(executable), str(corpus)]
    if args.standalone:
        return command
    command.extend(
        (
            f"-timeout={args.timeout}",
            f"-rss_limit_mb={args.rss_limit_mb}",
            "-print_final_stats=1",
            "-use_value_profile=1",
            f"-artifact_prefix={artifact_dir}{os.sep}",
        )
    )
    if args.max_len is not None:
        command.append(f"-max_len={args.max_len}")
    if args.max_total_time is None:
        command.append(f"-runs={args.runs}")
    else:
        command.append(f"-max_total_time={args.max_total_time}")
    if args.seed is not None:
        command.append(f"-seed={args.seed}")
    if args.dictionary is not None:
        command.append(f"-dict={args.dictionary}")
    return command


def build_report(
    *,
    args: argparse.Namespace,
    target: str,
    command: Sequence[str],
    executable: Path,
    environment: dict[str, str],
    revision: dict[str, Any],
    started_at: str,
    finished_at: str,
    elapsed_seconds: float,
    exit_code: int,
    execution_error: str | None,
    initial_corpus: dict[str, Any] | None,
    final_corpus: dict[str, Any] | None,
    artifacts: dict[str, Any] | None,
    log_path: Path,
    evidence_errors: list[str],
) -> dict[str, Any]:
    dictionary = None
    if args.dictionary is not None:
        dictionary = {
            "path": str(args.dictionary),
            "sha256": sha256_file(args.dictionary) if args.dictionary.is_file() else None,
        }
    return {
        "schema": EVIDENCE_SCHEMA,
        "status": (
            "passed"
            if exit_code == 0
            and execution_error is None
            and not evidence_errors
            and artifacts is not None
            and artifacts["fileCount"] == 0
            else "failed"
        ),
        "target": target,
        "mode": "corpus-replay" if args.standalone else "libFuzzer",
        "revision": revision,
        "timing": {
            "startedAtUtc": started_at,
            "finishedAtUtc": finished_at,
            "elapsedSeconds": round(elapsed_seconds, 3),
            "requestedMaxTotalTimeSeconds": args.max_total_time,
            "requestedRuns": None if args.max_total_time is not None else args.runs,
        },
        "configuration": {
            "command": list(command),
            "seed": args.seed,
            "maxInputLength": args.max_len,
            "sanitizers": args.sanitizer,
            "environment": {
                key: environment.get(key)
                for key in ("ASAN_OPTIONS", "UBSAN_OPTIONS", "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS")
            },
        },
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "description": platform.platform(),
        },
        "tools": collect_tool_versions(environment),
        "binary": {
            "path": str(executable),
            "sha256": sha256_file(executable) if executable.is_file() else None,
        },
        "dictionary": dictionary,
        "corpus": {"initial": initial_corpus, "final": final_corpus},
        "statistics": parse_final_stats(log_path),
        "artifacts": artifacts,
        "log": {"path": log_path.name, "sha256": sha256_file(log_path) if log_path.is_file() else None},
        "exitCode": exit_code,
        "executionError": execution_error,
        "evidenceErrors": evidence_errors,
    }


def run_with_evidence(
    args: argparse.Namespace,
    target: str,
    executable: Path,
    corpus: Path,
    artifact_dir: Path,
    command: Sequence[str],
    environment: dict[str, str],
    revision: dict[str, Any],
) -> int:
    assert args.evidence_dir is not None
    evidence = args.evidence_dir / target
    evidence.mkdir(parents=True, exist_ok=True)
    log_path = evidence / "run.log"
    report_path = evidence / "report.json"
    evidence_errors: list[str] = []
    initial_manifest: dict[str, Any] | None = None
    final_manifest: dict[str, Any] | None = None
    artifact_manifest: dict[str, Any] | None = None
    exit_code = 1
    execution_error: str | None = None
    report_write_failed = False
    execution_command = list(command)
    working_corpus = evidence / "working-corpus"

    started_at = datetime.now(timezone.utc).isoformat()
    started = time.monotonic()
    try:
        initial_manifest = snapshot_tree(
            corpus, evidence / "initial-corpus", evidence / "initial-corpus-manifest.json"
        )
        snapshot_tree(
            evidence / "initial-corpus", working_corpus, evidence / "working-corpus-manifest.json"
        )
        # libFuzzer may add, rename, or remove corpus files while it runs.  Keep
        # the checked-in/generated corpus immutable and archive the mutable copy
        # only after the child process has stopped.
        try:
            corpus_index = execution_command.index(str(corpus))
        except ValueError as error:
            raise ValueError("fuzz command does not contain its corpus argument") from error
        execution_command[corpus_index] = str(working_corpus)
        exit_code = tee_process(execution_command, environment, log_path)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        execution_error = f"{type(error).__name__}: {error}"
        print(f"Fuzz target {target} could not run: {execution_error}", file=sys.stderr)
    finally:
        try:
            final_manifest = snapshot_tree(
                working_corpus, evidence / "final-corpus", evidence / "final-corpus-manifest.json"
            )
        except (OSError, ValueError) as error:
            evidence_errors.append(f"final corpus snapshot: {error}")
        try:
            artifact_manifest = snapshot_tree(
                artifact_dir, evidence / "artifacts", evidence / "artifact-manifest.json"
            )
        except (OSError, ValueError) as error:
            evidence_errors.append(f"artifact snapshot: {error}")

        finished_at = datetime.now(timezone.utc).isoformat()
        elapsed_seconds = time.monotonic() - started
        try:
            report = build_report(
                args=args,
                target=target,
                command=execution_command,
                executable=executable,
                environment=environment,
                revision=revision,
                started_at=started_at,
                finished_at=finished_at,
                elapsed_seconds=elapsed_seconds,
                exit_code=exit_code,
                execution_error=execution_error,
                initial_corpus=initial_manifest,
                final_corpus=final_manifest,
                artifacts=artifact_manifest,
                log_path=log_path,
                evidence_errors=evidence_errors,
            )
            write_json(report_path, report)
        except OSError as error:
            print(f"Failed to write fuzz evidence report for {target}: {error}", file=sys.stderr)
            report_write_failed = True

    if report_write_failed or execution_error is not None or evidence_errors:
        return 1
    if artifact_manifest is not None and artifact_manifest["fileCount"] != 0:
        return exit_code if exit_code != 0 else 1
    return exit_code


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if (
        args.runs < 0
        or (args.max_len is not None and args.max_len <= 0)
        or (args.max_total_time is not None and args.max_total_time <= 0)
        or (args.seed is not None and args.seed < 0)
    ):
        print("runs and seed must be non-negative and time/max-len must be positive", file=sys.stderr)
        return 2
    if args.evidence_dir is not None:
        for label, source in (("corpus", args.corpus), ("artifact", args.artifacts)):
            if paths_overlap(args.evidence_dir, source):
                print(f"evidence and {label} directories must not overlap", file=sys.stderr)
                return 2

    revision = (
        revision_context(args.expected_sha)
        if args.expected_sha is not None or args.evidence_dir is not None
        else {}
    )
    if args.expected_sha is not None and not revision["matchesExpected"]:
        print(
            f"checked-out revision {revision['actualSha']} does not match expected candidate {args.expected_sha}",
            file=sys.stderr,
        )
        if args.evidence_dir is not None:
            write_json(args.evidence_dir / "revision-mismatch-report.json", revision)
        return 2

    environment = os.environ.copy()
    detect_leaks = "0" if sys.platform in ("win32", "darwin") else "1"
    environment.setdefault(
        "ASAN_OPTIONS", f"abort_on_error=1:detect_leaks={detect_leaks}:strict_string_checks=1"
    )
    environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    suffix = ".exe" if os.name == "nt" else ""

    for target in selected_targets(args):
        executable = args.binary_dir / f"{target}{suffix}"
        corpus = args.corpus / CORPUS_NAMES.get(target, target)
        artifact_dir = args.artifacts / target
        artifact_dir.mkdir(parents=True, exist_ok=True)
        if not executable.is_file():
            print(f"missing fuzz executable: {executable}", file=sys.stderr)
            return 1
        if not corpus.is_dir():
            print(f"missing fuzz corpus: {corpus}", file=sys.stderr)
            return 1

        command = build_command(args, executable, corpus, artifact_dir)
        print(f"Running {target} ({'corpus replay' if args.standalone else 'libFuzzer'})", flush=True)
        if args.evidence_dir is None:
            result = subprocess.run(command, env=environment, check=False)
            if result.returncode != 0:
                return result.returncode
            continue

        result = run_with_evidence(
            args, target, executable, corpus, artifact_dir, command, environment, revision
        )
        if result != 0:
            return result
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
