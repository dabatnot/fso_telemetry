#!/usr/bin/env python3
"""Focused contracts for multi-root telemetry fuzz corpus preparation."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("prepare_fuzz_corpus.py")
SPEC = importlib.util.spec_from_file_location("prepare_fuzz_corpus", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

REPOSITORY = SCRIPT.parents[4]
V11_VECTORS = REPOSITORY / "test/telemetry/protocol/vectors-v1.1"


class PrepareFuzzCorpusTests(unittest.TestCase):
    def test_includes_current_vector_root_with_seed_provenance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            manifest = MODULE.prepare([V11_VECTORS], Path(temporary) / "corpus")

        self.assertEqual(
            manifest["sourceVectors"],
            ["test/telemetry/protocol/vectors-v1.1"],
        )
        self.assertEqual(manifest["sourceVectorRoots"][0]["inputFileCount"], 21)
        sources = {
            source
            for target in manifest["targets"].values()
            for seed in target["seeds"].values()
            for source in seed["sources"]
        }
        self.assertTrue(any(source.startswith("vectors-v1.1/") for source in sources))

    def test_rejects_duplicate_vector_roots(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ValueError, "must be unique"):
                MODULE.prepare([V11_VECTORS, V11_VECTORS], Path(temporary) / "corpus")


if __name__ == "__main__":
    unittest.main()
