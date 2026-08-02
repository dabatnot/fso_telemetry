#!/usr/bin/env python3
"""Mechanical tests for the implementation tracker; never product evidence."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import phase_tracker


DELIVERY = """\
# 07 — Livraison et traçabilité produit

## Table canonique des exigences

| REQ | Invariant ou observable produit | Test automatisé | Observation manuelle éventuelle |
|---|---|---|---|
| `P1-REQ-001` | Le produit publie la valeur exacte. | `product_test` | `P1-OBS-01` |

## Observations neutres

| Scénario | Produit réellement observé | Attendu |
|---|---|---|
| `P1-OBS-01` | Le vrai produit est exécuté. | La valeur exacte est observée. |
"""


class PhaseTrackerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        analysis = self.root / "documentation" / "analysis"
        self.phase_directory = analysis / "specs" / "1-Test"
        self.phase_directory.mkdir(parents=True)
        (analysis / "README.md").write_text("# Active\n", encoding="utf-8")
        (self.phase_directory / "07-livraison-et-tracabilite.md").write_text(
            DELIVERY, encoding="utf-8"
        )
        (self.root / "code").mkdir()
        (self.root / "test").mkdir()
        (self.root / "code" / "product.cpp").write_text(
            "int product_value = 1;\n", encoding="utf-8"
        )
        (self.root / "test" / "product_test.cpp").write_text(
            "int expected_value = 1;\n", encoding="utf-8"
        )
        requirements, observations = phase_tracker.parse_contract(
            self.phase_directory, 1
        )
        self.tracker_path = (
            self.root
            / "documentation"
            / "analysis"
            / "implementation-status"
            / "phase-1.json"
        )
        self.tracker_path.parent.mkdir()
        self.tracker = phase_tracker.initial_tracker(
            self.root,
            1,
            self.phase_directory,
            requirements,
            observations,
        )
        self.requirements = requirements
        self.observations = observations

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_tracker(self) -> None:
        self.tracker_path.write_text(
            json.dumps(self.tracker, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    def complete_tracker(self) -> None:
        inputs = ["code/product.cpp", "test/product_test.cpp"]
        fingerprint, _ = phase_tracker.fingerprint_inputs(self.root, inputs)
        requirement = self.tracker["requirements"][0]
        requirement["implementationStatus"] = "implemented"
        requirement["implementationSummary"] = "La valeur exacte est publiée."
        requirement["implementationPaths"] = ["code/product.cpp"]
        proof = requirement["proofs"][0]
        proof.update(
            {
                "command": "product_test",
                "status": "passed",
                "inputs": inputs,
                "inputFingerprint": fingerprint,
                "observedAtUtc": "2026-08-01T12:00:00Z",
                "result": "1 test passed; exact value observed",
            }
        )
        observation = self.tracker["observations"][0]
        observation.update(
            {
                "status": "passed",
                "inputs": inputs,
                "inputFingerprint": fingerprint,
                "observedAtUtc": "2026-08-01T12:05:00Z",
                "reviewedBy": "human-reviewer",
                "observed": "La valeur exacte est observée.",
                "deviation": "aucun",
                "impact": "aucun",
            }
        )

    def validate(self) -> dict:
        self.write_tracker()
        return phase_tracker.validate_tracker(
            self.root,
            self.tracker_path,
            1,
            self.phase_directory,
            self.requirements,
            self.observations,
        )

    def test_pending_tracker_is_valid_but_incomplete(self) -> None:
        result = self.validate()
        self.assertEqual("incomplete", result["derivedStatus"])
        self.assertFalse(result["errors"])
        self.assertTrue(result["gaps"])

    def test_complete_is_derived_from_current_evidence(self) -> None:
        self.complete_tracker()
        result = self.validate()
        self.assertEqual("complete", result["derivedStatus"])
        self.assertFalse(result["errors"])
        self.assertFalse(result["gaps"])

    def test_source_change_makes_evidence_stale(self) -> None:
        self.complete_tracker()
        self.write_tracker()
        (self.root / "code" / "product.cpp").write_text(
            "int product_value = 2;\n", encoding="utf-8"
        )
        result = phase_tracker.validate_tracker(
            self.root,
            self.tracker_path,
            1,
            self.phase_directory,
            self.requirements,
            self.observations,
        )
        self.assertEqual("incomplete", result["derivedStatus"])
        self.assertTrue(
            any("input fingerprint is stale" in gap for gap in result["gaps"])
        )

    def test_unknown_field_is_rejected(self) -> None:
        self.tracker["unexpected"] = True
        result = self.validate()
        self.assertEqual("invalid", result["derivedStatus"])
        self.assertTrue(any("unknown fields" in error for error in result["errors"]))


if __name__ == "__main__":
    unittest.main()
