from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path


TOOL_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = TOOL_ROOT.parents[1]
BUILD_ROOT = REPO_ROOT / "build" / "av-core"


def read_version() -> str:
    source = (TOOL_ROOT / "backend" / "av_core" / "__init__.py").read_text(encoding="utf-8")
    match = re.search(r'^__version__\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"$', source, re.MULTILINE)
    if match is None:
        raise RuntimeError("AV CORE version was not found")
    return match.group(1)


def run_checks() -> None:
    subprocess.run(
        [sys.executable, "-m", "unittest", "discover", "-s", str(TOOL_ROOT / "tests" / "backend"), "-p", "test_*.py"],
        cwd=REPO_ROOT,
        check=True,
    )
    npm = "npm.cmd" if sys.platform == "win32" else "npm"
    subprocess.run([npm, "ci"], cwd=TOOL_ROOT / "frontend", check=True)
    subprocess.run([npm, "test", "--", "--run"], cwd=TOOL_ROOT / "frontend", check=True)
    subprocess.run([npm, "run", "build"], cwd=TOOL_ROOT / "frontend", check=True)


def _copy_tree(source: Path, destination: Path) -> None:
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"))


def assemble_release(destination: Path, frontend_dist: Path, version: str) -> None:
    destination.mkdir(parents=True)
    _copy_tree(TOOL_ROOT / "backend" / "av_core", destination / "backend" / "av_core")
    shutil.copy2(TOOL_ROOT / "backend" / "requirements.lock.txt", destination / "requirements.lock.txt")
    _copy_tree(frontend_dist, destination / "frontend")
    fstl_destination = destination / "fstl-client"
    fstl_destination.mkdir()
    for name in ("fstl_client_core.py", "fstl_reference_decoder.py"):
        shutil.copy2(REPO_ROOT / "test" / "telemetry" / "protocol" / "tools" / name, fstl_destination / name)
    for name in (
        "install.sh", "av-core.service", "av-core-can.service", "av-core-can-up",
        "smoke-test-vcan.sh", "OPERATIONS.md",
    ):
        shutil.copy2(TOOL_ROOT / "packaging" / name, destination / name)
    (destination / "VERSION").write_bytes(f"{version}\n".encode("ascii"))


def _archive_metadata(info: tarfile.TarInfo) -> tarfile.TarInfo:
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = 0
    if info.isdir():
        info.mode = 0o755
    elif Path(info.name).name in {"install.sh", "av-core-can-up", "smoke-test-vcan.sh"}:
        info.mode = 0o755
    else:
        info.mode = 0o644
    return info


def build_archive(*, skip_checks: bool = False) -> Path:
    version = read_version()
    if not skip_checks:
        run_checks()
    frontend_dist = BUILD_ROOT / "frontend"
    if not (frontend_dist / "index.html").is_file():
        raise RuntimeError("compiled frontend is missing; run without --skip-checks")
    BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    archive = BUILD_ROOT / f"av-core-{version}.tar.gz"
    with tempfile.TemporaryDirectory(prefix="av-core-release-", dir=BUILD_ROOT) as temporary:
        release_root = Path(temporary) / f"av-core-{version}"
        assemble_release(release_root, frontend_dist, version)
        with tarfile.open(archive, "w:gz", format=tarfile.PAX_FORMAT) as output:
            output.add(release_root, arcname=release_root.name, filter=_archive_metadata)
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description="Build a self-contained AV CORE Raspberry Pi release archive")
    parser.add_argument("--skip-checks", action="store_true", help="reuse an already verified compiled frontend")
    args = parser.parse_args()
    print(build_archive(skip_checks=args.skip_checks))


if __name__ == "__main__":
    main()
