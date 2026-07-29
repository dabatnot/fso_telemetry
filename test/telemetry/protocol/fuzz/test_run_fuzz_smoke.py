#!/usr/bin/env python3
"""Unit tests for the telemetry fuzz campaign evidence runner."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import run_fuzz_smoke as runner


class FuzzSmokeRunnerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def arguments(self, *extra: str) -> runner.argparse.Namespace:
        return runner.parse_args(
            [
                "--binary-dir",
                str(self.root / "bin"),
                "--corpus",
                str(self.root / "corpus"),
                "--artifacts",
                str(self.root / "artifacts"),
                *extra,
            ]
        )

    def write_fake_fuzzer(self) -> Path:
        script = self.root / "fake_fuzzer.py"
        script.write_text(
            """\
from pathlib import Path
import sys

corpus = Path(sys.argv[1])
artifacts = Path(sys.argv[2])
exit_code = int(sys.argv[3])
(corpus / "initial-seed").unlink()
(corpus / "generated-seed").write_bytes(b"mutated corpus seed")
print("fake fuzzer output")
print("stat::number_of_executed_units: 321")
print("stat::average_exec_per_sec: 123.5")
if exit_code:
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts / "crash-deadbeef").write_bytes(b"crashing input")
raise SystemExit(exit_code)
""",
            encoding="utf-8",
        )
        return script

    def prepare_campaign(self, exit_code: int) -> tuple[runner.argparse.Namespace, str, Path, Path, list[str]]:
        target = "fuzz_packet_reader"
        corpus = self.root / "corpus" / target
        artifact_dir = self.root / "artifacts" / target
        corpus.mkdir(parents=True)
        artifact_dir.mkdir(parents=True)
        (corpus / "initial-seed").write_bytes(b"golden vector seed")
        fake_fuzzer = self.write_fake_fuzzer()
        args = self.arguments(
            "--target",
            target,
            "--max-total-time",
            "1800",
            "--seed",
            "4242",
            "--sanitizer",
            "fuzzer",
            "--sanitizer",
            "address",
            "--sanitizer",
            "undefined",
            "--evidence-dir",
            str(self.root / "evidence"),
        )
        command = [sys.executable, str(fake_fuzzer), str(corpus), str(artifact_dir), str(exit_code)]
        return args, target, corpus, artifact_dir, command

    def test_default_and_single_target_selection(self) -> None:
        default_args = self.arguments()
        self.assertEqual(2000, default_args.runs)
        self.assertIsNone(default_args.max_len)
        self.assertEqual(runner.TARGETS, runner.selected_targets(default_args))

        selected_args = self.arguments("--target", "fuzz_records")
        self.assertEqual(("fuzz_records",), runner.selected_targets(selected_args))

    def test_max_len_is_omitted_unless_explicitly_requested(self) -> None:
        corpus = self.root / "corpus"
        artifacts = self.root / "artifacts"

        default_command = runner.build_command(
            self.arguments(), self.root / "fuzzer", corpus, artifacts
        )
        self.assertFalse(any(argument.startswith("-max_len=") for argument in default_command))

        explicit_command = runner.build_command(
            self.arguments("--max-len", "4096"),
            self.root / "fuzzer",
            corpus,
            artifacts,
        )
        self.assertIn("-max_len=4096", explicit_command)

    def test_tree_manifest_is_stable_and_content_sensitive(self) -> None:
        first = self.root / "first"
        second = self.root / "second"
        (first / "nested").mkdir(parents=True)
        (second / "nested").mkdir(parents=True)
        (first / "z-seed").write_bytes(b"z")
        (first / "nested" / "a-seed").write_bytes(b"a")
        (second / "nested" / "a-seed").write_bytes(b"a")
        (second / "z-seed").write_bytes(b"z")

        first_manifest = runner.tree_manifest(first)
        second_manifest = runner.tree_manifest(second)
        self.assertEqual(first_manifest, second_manifest)

        os.utime(second / "z-seed", None)
        self.assertEqual(first_manifest["treeSha256"], runner.tree_manifest(second)["treeSha256"])
        (second / "z-seed").write_bytes(b"changed")
        self.assertNotEqual(first_manifest["treeSha256"], runner.tree_manifest(second)["treeSha256"])

    def run_fake_campaign(self, exit_code: int) -> tuple[int, dict[str, object], Path]:
        args, target, corpus, artifact_dir, command = self.prepare_campaign(exit_code)
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "abort_on_error=1:detect_leaks=1:strict_string_checks=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        revision = {
            "expectedSha": "a" * 40,
            "actualSha": "a" * 40,
            "matchesExpected": True,
        }
        with mock.patch.object(
            runner,
            "collect_tool_versions",
            return_value={"python": {"executable": sys.executable, "version": sys.version}},
        ):
            result = runner.run_with_evidence(
                args,
                target,
                Path(sys.executable),
                corpus,
                artifact_dir,
                command,
                environment,
                revision,
            )
        evidence = self.root / "evidence" / target
        report = json.loads((evidence / "report.json").read_text(encoding="utf-8"))
        return result, report, evidence

    def test_success_report_contains_log_stats_revision_and_corpora(self) -> None:
        result, report, evidence = self.run_fake_campaign(0)

        self.assertEqual(0, result)
        self.assertEqual("passed", report["status"])
        self.assertEqual("a" * 40, report["revision"]["actualSha"])
        self.assertEqual(1800, report["timing"]["requestedMaxTotalTimeSeconds"])
        self.assertGreaterEqual(report["timing"]["elapsedSeconds"], 0)
        self.assertEqual(4242, report["configuration"]["seed"])
        self.assertEqual(
            ["fuzzer", "address", "undefined"], report["configuration"]["sanitizers"]
        )
        self.assertIn("detect_leaks=1", report["configuration"]["environment"]["ASAN_OPTIONS"])
        self.assertEqual(1, report["corpus"]["initial"]["fileCount"])
        self.assertEqual(1, report["corpus"]["final"]["fileCount"])
        self.assertNotEqual(
            report["corpus"]["initial"]["treeSha256"], report["corpus"]["final"]["treeSha256"]
        )
        self.assertTrue((self.root / "corpus" / "fuzz_packet_reader" / "initial-seed").is_file())
        self.assertFalse((self.root / "corpus" / "fuzz_packet_reader" / "generated-seed").exists())
        self.assertIn(str(evidence / "working-corpus"), report["configuration"]["command"])
        self.assertEqual(321, report["statistics"]["number_of_executed_units"])
        self.assertEqual(123.5, report["statistics"]["average_exec_per_sec"])
        self.assertEqual(0, report["artifacts"]["fileCount"])
        self.assertIn("fake fuzzer output", (evidence / "run.log").read_text(encoding="utf-8"))
        self.assertTrue((evidence / "initial-corpus-manifest.json").is_file())
        self.assertTrue((evidence / "final-corpus-manifest.json").is_file())

    def test_failure_still_writes_report_and_archives_crash(self) -> None:
        result, report, evidence = self.run_fake_campaign(23)

        self.assertEqual(23, result)
        self.assertEqual("failed", report["status"])
        self.assertEqual(23, report["exitCode"])
        self.assertEqual(1, report["artifacts"]["fileCount"])
        self.assertEqual("crash-deadbeef", report["artifacts"]["files"][0]["path"])
        self.assertTrue((evidence / "artifacts" / "crash-deadbeef").is_file())
        self.assertTrue((evidence / "report.json").is_file())


if __name__ == "__main__":
    unittest.main()
