from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "packaging" / "build_release.py"
SPEC = importlib.util.spec_from_file_location("av_core_build_release", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
build_release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_release)


class ReleaseBundleTest(unittest.TestCase):
    def test_release_contains_runtime_only(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            frontend = root / "compiled-frontend"
            frontend.mkdir()
            (frontend / "index.html").write_text("<html>AV CORE</html>", encoding="utf-8")
            (frontend / "assets").mkdir()
            (frontend / "assets" / "app.js").write_text("// compiled", encoding="utf-8")
            destination = root / "release"
            build_release.assemble_release(destination, frontend, build_release.read_version())

            expected = {
                "VERSION", "backend", "frontend", "fstl-client", "requirements.lock.txt",
                "install.sh", "av-core.service", "av-core-can.service", "av-core-can-up",
                "smoke-test-vcan.sh", "OPERATIONS.md",
            }
            self.assertEqual(expected, {entry.name for entry in destination.iterdir()})
            self.assertTrue((destination / "frontend" / "index.html").is_file())
            self.assertTrue((destination / "backend" / "av_core" / "app.py").is_file())
            self.assertFalse(any(path.name in {"node_modules", "package.json", "tests", "firmware"} for path in destination.rglob("*")))
            self.assertFalse(any(path.suffix in {".pyc", ".pyo"} for path in destination.rglob("*")))

    def test_installer_preserves_state_and_requires_complete_bundle(self) -> None:
        installer = (SCRIPT.parent / "install.sh").read_text(encoding="utf-8")
        self.assertIn("/var/lib/fsotelemetry", installer)
        self.assertNotIn('rm -rf -- "/var/lib/fsotelemetry', installer)
        self.assertIn("frontend/index.html", installer)
        self.assertIn("--set-hostname", installer)


if __name__ == "__main__":
    unittest.main()
