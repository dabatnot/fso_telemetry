#!/usr/bin/env python3
"""Independent contract for the frozen FSTL 1.0 / additive 1.1 split.

This test owns independent hashes for the machine-readable wire artifacts.
Product Markdown remains editable and is not used as a protocol oracle.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
PROTOCOL_ROOT = REPO_ROOT / "test" / "telemetry" / "protocol"

FROZEN_SCHEMA_BYTES = 499_494
FROZEN_SCHEMA_SHA256 = "a870d85fdb0070d61a57e40d7d2b1183a9a83458cedf446708511fcaac5134cc"
FROZEN_MACHINE_FILE_COUNT = 430
FROZEN_MACHINE_TREE_SHA256 = "6174ef30453a25ff4810e7253343c84fcce787e445361f1aeb7b1d8964f5a2eb"
FROZEN_IMMUTABLE_FILE_COUNT = 431
FROZEN_IMMUTABLE_TREE_SHA256 = "bdca3b1e3b317b44abb9e4869041767e6edf499acb728728b2eeaa8892300ea6"
FROZEN_FULL_FILE_COUNT = 438
FROZEN_FULL_TREE_SHA256 = "9baac6a20db33bcf350066ed533c5581b7117410899d7bc4a6dc24406e47856d"
FROZEN_LEDGER_BYTES = 131_635
FROZEN_LEDGER_SHA256 = "800bc258719b7992b1ae38a30d8dc1f8a7373a5dd0415a618a0d4e62a47dbc92"
FROZEN_LAYOUT_SHA256 = "fd99d8265e81307529d0c6ba2a528e95e95a5cd3aaff0ceb142edf27bd3cc6b4"
FROZEN_ENCODED_PROBE_SHA256 = "ee45ad75728442145aa2689211f237868f095a39a2735b89ae5868c3dbe95438"
FROZEN_LEDGER = PROTOCOL_ROOT / "fstl-1.0-artifacts.manifest.json"
FREEZE_VERIFIER = PROTOCOL_ROOT / "tools" / "verify_fstl_1_0_freeze.py"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_tree_sha256(entries: list[tuple[str, bytes]]) -> str:
    body = b"".join(
        path.encode("utf-8") + b"\0" + sha256(data).encode("ascii") + b"\n"
        for path, data in sorted(entries)
    )
    return sha256(body)


def relative(path: Path) -> str:
    return path.relative_to(REPO_ROOT).as_posix()


def machine_entries() -> list[tuple[str, bytes]]:
    entries: list[tuple[str, bytes]] = []
    for root_name in ("vectors", "expected"):
        root = PROTOCOL_ROOT / root_name
        entries.extend((relative(path), path.read_bytes()) for path in root.rglob("*") if path.is_file())
    for name in (
        "protocol-vectors.manifest.json",
        "transport-vectors.manifest.json",
        "protocol-coverage.json",
        "transport-harness-seeds.json",
        "fuzz/fstl.dict",
    ):
        path = PROTOCOL_ROOT / name
        entries.append((relative(path), path.read_bytes()))
    return sorted(entries)


def all_frozen_entries() -> list[tuple[str, bytes]]:
    schema = PROTOCOL_ROOT / "schema" / "fstl-v1.yaml"
    return sorted([(relative(schema), schema.read_bytes())] + machine_entries())


def copy_frozen_contract(destination: Path) -> None:
    for name, data in all_frozen_entries():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    if FROZEN_LEDGER.is_file():
        target = destination / relative(FROZEN_LEDGER)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(FROZEN_LEDGER, target)


def run_freeze_verifier(repo: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(FREEZE_VERIFIER), "--check", "--repo", str(repo)],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
    )


class FstlFrozenContractRedTest(unittest.TestCase):
    maxDiff = None

    def test_frz_001_historical_v10_ledger_is_byte_frozen(self) -> None:
        data = FROZEN_LEDGER.read_bytes()
        self.assertEqual(len(data), FROZEN_LEDGER_BYTES)
        self.assertEqual(sha256(data), FROZEN_LEDGER_SHA256)

    def test_frz_002_phase0_schema_is_exact_frozen_artifact(self) -> None:
        path = PROTOCOL_ROOT / "schema" / "fstl-v1.yaml"
        data = path.read_bytes()
        self.assertEqual(len(data), FROZEN_SCHEMA_BYTES, "P0-AC-025 changed fstl-v1.yaml byte length")
        self.assertEqual(sha256(data), FROZEN_SCHEMA_SHA256, "P0-AC-025 changed fstl-v1.yaml SHA-256")
        schema = json.loads(data)
        self.assertEqual(schema.get("wire_version"), "1.0")
        self.assertNotIn("PLAYER_KINEMATICS", data.decode("utf-8"))

    def test_frz_003_v10_retains_its_historical_document_provenance(self) -> None:
        schema = json.loads((PROTOCOL_ROOT / "schema" / "fstl-v1.yaml").read_bytes())
        sources = schema.get("normative_sources")
        self.assertIsInstance(sources, list)
        self.assertEqual(len(sources), 7)
        for entry in sources:
            with self.subTest(source=entry):
                self.assertRegex(entry.get("sha256", ""), r"^[0-9a-f]{64}$")

    def test_frz_004_machine_artifact_set_is_complete_and_frozen(self) -> None:
        entries = machine_entries()
        self.assertEqual(len(entries), FROZEN_MACHINE_FILE_COUNT)
        self.assertEqual(canonical_tree_sha256(entries), FROZEN_MACHINE_TREE_SHA256)

    def test_frz_005_historical_v10_layout_locks_cannot_move(self) -> None:
        checker = (PROTOCOL_ROOT / "tools" / "verify_schema_vectors.py").read_text(encoding="utf-8")
        layout = re.search(
            r'^EXPECTED_LAYOUT_SHA256_V1_0\s*=\s*"([0-9a-f]{64})"', checker, re.MULTILINE
        ) or re.search(r'^EXPECTED_LAYOUT_SHA256\s*=\s*"([0-9a-f]{64})"', checker, re.MULTILINE)
        encoded = re.search(
            r'^EXPECTED_ENCODED_PROBE_SHA256_V1_0\s*=\s*"([0-9a-f]{64})"', checker, re.MULTILINE
        ) or re.search(r'^EXPECTED_ENCODED_PROBE_SHA256\s*=\s*"([0-9a-f]{64})"', checker, re.MULTILINE)
        self.assertIsNotNone(layout, "v1.0 layout lock disappeared")
        self.assertIsNotNone(encoded, "v1.0 encoded-probe lock disappeared")
        assert layout is not None and encoded is not None
        self.assertEqual(layout.group(1), FROZEN_LAYOUT_SHA256)
        self.assertEqual(encoded.group(1), FROZEN_ENCODED_PROBE_SHA256)

    def test_frz_006_v11_has_distinct_schema_and_informative_provenance(self) -> None:
        v10_path = PROTOCOL_ROOT / "schema" / "fstl-v1.yaml"
        v11_path = PROTOCOL_ROOT / "schema" / "fstl-v1.1.yaml"
        self.assertTrue(v11_path.is_file(), "P0.13 requires a distinct schema/fstl-v1.1.yaml")
        v10_data = v10_path.read_bytes()
        v11_data = v11_path.read_bytes()
        self.assertNotEqual(v10_data, v11_data)
        schema = json.loads(v11_data)
        self.assertEqual(schema.get("wire_version"), "1.1")
        self.assertIn("PLAYER_KINEMATICS", v11_data.decode("utf-8"))
        self.assertNotIn("normative_sources", schema)
        self.assertNotIn("normative_document_set", schema)
        provenance = schema.get("amendment_provenance")
        self.assertIsInstance(provenance, dict)
        self.assertIs(provenance.get("normative"), False)
        entries = [
            entry
            for group, values in provenance.items()
            if group != "normative"
            for entry in values
        ]
        self.assertTrue(entries)
        for entry in entries:
            with self.subTest(provenance=entry):
                self.assertIn("document", entry)
                self.assertIn("requirements", entry)
                self.assertNotIn("sha256", entry)

    def test_frz_007_versioned_ledger_covers_the_immutable_v10_artifacts(self) -> None:
        self.assertTrue(FROZEN_LEDGER.is_file(), "missing in-tree FSTL 1.0 frozen-artifact ledger")
        manifest = json.loads(FROZEN_LEDGER.read_bytes())
        self.assertEqual(manifest.get("schema"), "FSTL-1.0-FROZEN-ARTIFACTS")
        self.assertEqual(manifest.get("wireVersion"), "1.0")
        self.assertEqual(manifest.get("fileCount"), FROZEN_FULL_FILE_COUNT)
        self.assertEqual(manifest.get("treeSha256"), FROZEN_FULL_TREE_SHA256)

        files = manifest.get("files")
        self.assertIsInstance(files, list)
        paths = [entry.get("path") for entry in files]
        self.assertEqual(paths, sorted(paths), "ledger paths must be canonical and sorted")
        self.assertEqual(len(paths), len(set(paths)), "ledger paths must be unique")

        expected = {path: data for path, data in all_frozen_entries()}
        declared = {entry["path"]: entry for entry in files}
        self.assertTrue(set(expected).issubset(declared), "immutable artifact missing from historical ledger")
        for path, data in expected.items():
            with self.subTest(artifact=path):
                self.assertEqual(declared[path].get("bytes"), len(data))
                self.assertEqual(declared[path].get("sha256"), sha256(data))

        entries = sorted(expected.items())
        self.assertEqual(len(entries), FROZEN_IMMUTABLE_FILE_COUNT)
        self.assertEqual(canonical_tree_sha256(entries), FROZEN_IMMUTABLE_TREE_SHA256)

    def test_frz_008_freeze_verifier_owns_an_independent_pinned_oracle(self) -> None:
        self.assertTrue(FREEZE_VERIFIER.is_file(), "missing production verify_fstl_1_0_freeze.py")
        self.assertTrue(FROZEN_LEDGER.is_file(), "missing in-tree FSTL 1.0 ledger")
        source = FREEZE_VERIFIER.read_text(encoding="utf-8")
        self.assertIn(
            FROZEN_IMMUTABLE_TREE_SHA256,
            source,
            "the verifier must pin an oracle independent of the mutable ledger/current tree",
        )
        self.assertIn(FROZEN_LEDGER_SHA256, source)
        result = run_freeze_verifier(REPO_ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(str(FROZEN_IMMUTABLE_FILE_COUNT), result.stdout)
        self.assertIn(FROZEN_IMMUTABLE_TREE_SHA256, result.stdout)

    def test_frz_009_freeze_verifier_rejects_each_artifact_class_and_inventory_drift(self) -> None:
        self.assertTrue(FREEZE_VERIFIER.is_file(), "missing production verify_fstl_1_0_freeze.py")
        self.assertTrue(FROZEN_LEDGER.is_file(), "missing in-tree FSTL 1.0 ledger")
        mutation_paths = (
            "test/telemetry/protocol/schema/fstl-v1.yaml",
            "test/telemetry/protocol/vectors/valid/messages/ack/ack.bin",
            "test/telemetry/protocol/expected/messages/ack.json",
            "test/telemetry/protocol/protocol-vectors.manifest.json",
            "test/telemetry/protocol/transport-vectors.manifest.json",
            "test/telemetry/protocol/protocol-coverage.json",
            "test/telemetry/protocol/transport-harness-seeds.json",
            "test/telemetry/protocol/fuzz/fstl.dict",
        )
        with tempfile.TemporaryDirectory(prefix="fstl-v10-freeze-red-") as directory:
            root = Path(directory)
            copy_frozen_contract(root)
            baseline = run_freeze_verifier(root)
            self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)

            for name in mutation_paths:
                with self.subTest(mutation=name):
                    path = root / name
                    original = path.read_bytes()
                    path.write_bytes(original + b"\nFSTL_FREEZE_MUTATION")
                    result = run_freeze_verifier(root)
                    path.write_bytes(original)
                    self.assertNotEqual(result.returncode, 0, f"mutation escaped verifier: {name}")
                    self.assertIn(name, result.stdout + result.stderr)

            added = root / "test/telemetry/protocol/vectors/unexpected-freeze-bypass.bin"
            added.write_bytes(b"unexpected")
            result = run_freeze_verifier(root)
            added.unlink()
            self.assertNotEqual(result.returncode, 0, "unexpected frozen-root file escaped verifier")
            self.assertIn(
                "test/telemetry/protocol/vectors/unexpected-freeze-bypass.bin",
                result.stdout + result.stderr,
            )

            removed = root / "test/telemetry/protocol/expected/messages/ack.json"
            original = removed.read_bytes()
            removed.unlink()
            result = run_freeze_verifier(root)
            removed.parent.mkdir(parents=True, exist_ok=True)
            removed.write_bytes(original)
            self.assertNotEqual(result.returncode, 0, "missing frozen file escaped verifier")
            self.assertIn("immutable FSTL 1.0 artifact drift", result.stdout + result.stderr)

    def test_frz_010_colluding_tree_and_ledger_update_cannot_refresh_the_oracle(self) -> None:
        self.assertTrue(FREEZE_VERIFIER.is_file(), "missing production verify_fstl_1_0_freeze.py")
        self.assertTrue(FROZEN_LEDGER.is_file(), "missing in-tree FSTL 1.0 ledger")
        with tempfile.TemporaryDirectory(prefix="fstl-v10-oracle-red-") as directory:
            root = Path(directory)
            copy_frozen_contract(root)
            schema_path = root / "test/telemetry/protocol/schema/fstl-v1.yaml"
            schema_path.write_bytes(schema_path.read_bytes() + b"\nCOLLUDING_MUTATION")

            ledger_path = root / relative(FROZEN_LEDGER)
            ledger = json.loads(ledger_path.read_bytes())
            schema_name = "test/telemetry/protocol/schema/fstl-v1.yaml"
            for entry in ledger["files"]:
                if entry["path"] == schema_name:
                    entry["bytes"] = schema_path.stat().st_size
                    entry["sha256"] = sha256(schema_path.read_bytes())
                    break
            else:
                self.fail("test precondition: schema missing from frozen ledger")
            ledger["treeSha256"] = canonical_tree_sha256(
                [(name, (root / name).read_bytes()) for name, _ in all_frozen_entries()]
            )
            ledger_path.write_text(json.dumps(ledger, indent=2, sort_keys=True) + "\n", encoding="utf-8")

            result = run_freeze_verifier(root)
            self.assertNotEqual(result.returncode, 0, "tree+ledger collusion refreshed the frozen oracle")
            self.assertIn(FROZEN_LEDGER_SHA256, result.stdout + result.stderr)

    def test_frz_011_mutated_ledger_is_rejected_before_use(self) -> None:
        self.assertTrue(FREEZE_VERIFIER.is_file(), "missing production verify_fstl_1_0_freeze.py")
        self.assertTrue(FROZEN_LEDGER.is_file(), "missing in-tree FSTL 1.0 ledger")
        for label, injected in (
            ("duplicate", None),
            ("traversal", {"path": "../outside", "bytes": 0, "sha256": sha256(b"")}),
        ):
            with self.subTest(bypass=label), tempfile.TemporaryDirectory(prefix="fstl-v10-ledger-red-") as directory:
                root = Path(directory)
                copy_frozen_contract(root)
                ledger_path = root / relative(FROZEN_LEDGER)
                ledger = json.loads(ledger_path.read_bytes())
                ledger["files"].append(dict(ledger["files"][0]) if injected is None else injected)
                ledger["fileCount"] = len(ledger["files"])
                ledger_path.write_text(json.dumps(ledger, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                result = run_freeze_verifier(root)
                self.assertNotEqual(result.returncode, 0, f"{label} ledger path escaped verifier")
                self.assertIn("frozen ledger identity drift", (result.stdout + result.stderr).lower())

    def test_frz_012_amendment_manifest_references_the_complete_v10_ledger(self) -> None:
        amendment = json.loads((PROTOCOL_ROOT / "fstl-1.1-vectors.manifest.json").read_bytes())
        self.assertNotIn(
            "frozenV10TreeSha256",
            amendment,
            "legacy field hashes only vectors/** and must not claim to freeze all v1.0 artifacts",
        )
        self.assertEqual(
            amendment.get("frozenV10ArtifactsManifest"),
            "fstl-1.0-artifacts.manifest.json",
        )
        self.assertEqual(amendment.get("frozenV10ArtifactCount"), FROZEN_FULL_FILE_COUNT)
        self.assertEqual(amendment.get("frozenV10ArtifactsTreeSha256"), FROZEN_FULL_TREE_SHA256)

    def test_route_001_python_consumers_name_the_schema_for_each_version(self) -> None:
        expectations = {
            "fstl_schema.py": ("fstl-v1.yaml", "fstl-v1.1.yaml"),
            "verify_schema_vectors.py": ("fstl-v1.yaml", "fstl-v1.1.yaml"),
            "verify_telemetry_assets.py": ("fstl-v1.yaml", "fstl-v1.1.yaml"),
            "fstl_reference_decoder.py": ("fstl-v1.yaml", "fstl-v1.1.yaml"),
            "verify_fstl_1_1_amendment.py": ("fstl-v1.1.yaml",),
        }
        for script_name, required_literals in expectations.items():
            source = (PROTOCOL_ROOT / "tools" / script_name).read_text(encoding="utf-8")
            for literal in required_literals:
                with self.subTest(script=script_name, route=literal):
                    self.assertIn(literal, source)

        for script_name in ("generate_protocol_vectors.py", "generate_transport_vectors.py"):
            source = (PROTOCOL_ROOT / "tools" / script_name).read_text(encoding="utf-8")
            with self.subTest(base_generator=script_name):
                self.assertIn("FSTL-1.0", source)
                self.assertNotIn("FSTL-1.1", source)

    def test_route_002_cpp_defaults_remain_v10_and_v11_is_explicit_opt_in(self) -> None:
        constants = (REPO_ROOT / "code" / "telemetry" / "protocol" / "telemetry_protocol_constants.h").read_text(
            encoding="utf-8"
        )
        required = (
            "VersionMinorV1_0 = 0",
            "VersionMinorV1_1 = 1",
            "VersionMinor = VersionMinorV1_0",
            "FrozenV1_0MinorRange{VersionMinorV1_0, VersionMinorV1_0}",
            "Phase1ProducerMinorRange{VersionMinorV1_1, VersionMinorV1_1}",
            "StateDomainCoverageBitPlayerKinematics = 0x0400ULL",
            "KnownStateDomainCoverageBitsV1_0 = 0x03efULL",
            "KnownStateDomainCoverageBitsV1_1 = 0x07efULL",
            "KnownStateDomainCoverageBits = KnownStateDomainCoverageBitsV1_0",
        )
        for invariant in required:
            with self.subTest(cpp_invariant=invariant):
                self.assertIn(invariant, constants)

    def test_route_003_amendment_and_decoders_fail_on_v11_schema_drift(self) -> None:
        v11_schema = PROTOCOL_ROOT / "schema" / "fstl-v1.1.yaml"
        self.assertTrue(v11_schema.is_file(), "routing mutation requires the distinct 1.1 schema")
        with tempfile.TemporaryDirectory(prefix="fstl-v11-route-red-") as directory:
            root = Path(directory)
            copied_protocol = root / relative(PROTOCOL_ROOT)
            shutil.copytree(PROTOCOL_ROOT, copied_protocol)

            constants = REPO_ROOT / "code/telemetry/protocol/telemetry_protocol_constants.h"
            copied_constants = root / relative(constants)
            copied_constants.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(constants, copied_constants)

            catalogue_sources = REPO_ROOT / "test/src/telemetry/protocol"
            shutil.copytree(catalogue_sources, root / relative(catalogue_sources))

            commands = (
                (
                    "verify_fstl_1_1_amendment.py",
                    ["--check", "--repo", str(root)],
                ),
                (
                    "fstl_reference_decoder.py",
                    ["--check", "--repo", str(root)],
                ),
                (
                    "verify_telemetry_assets.py",
                    ["--repo", str(root), "--require-complete"],
                ),
            )
            for script_name, arguments in commands:
                with self.subTest(baseline=script_name):
                    result = subprocess.run(
                        [sys.executable, "-B", str(PROTOCOL_ROOT / "tools" / script_name), *arguments],
                        cwd=REPO_ROOT,
                        check=False,
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            copied_schema = root / relative(v11_schema)
            schema = json.loads(copied_schema.read_bytes())
            values = schema["numeric_registries"]["StateDomainCoverage"]["values"]
            player = next(value for value in values if value.get("name") == "PLAYER_KINEMATICS")
            player["value"] = 0x800
            copied_schema.write_text(json.dumps(schema, indent=2, sort_keys=True) + "\n", encoding="utf-8")

            for script_name, arguments in commands:
                with self.subTest(mutated_v11_schema=script_name):
                    result = subprocess.run(
                        [sys.executable, "-B", str(PROTOCOL_ROOT / "tools" / script_name), *arguments],
                        cwd=REPO_ROOT,
                        check=False,
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                    )
                    self.assertNotEqual(result.returncode, 0, f"{script_name} ignored fstl-v1.1.yaml drift")
                    self.assertRegex(result.stdout + result.stderr, r"(?i)(fstl-v1\.1|player.kinematics|schema)")


if __name__ == "__main__":
    unittest.main(verbosity=2)
