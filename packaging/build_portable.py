#!/usr/bin/env python3
"""Build a portable PyInstaller executable for Linux or macOS."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


SUPPORTED_PLATFORMS = {"linux", "darwin"}
REPO_ROOT = Path(__file__).resolve().parent.parent
ENTRYPOINT = REPO_ROOT / "wii_rip.py"


def main() -> None:
    args = parse_args()
    platform = sys.platform
    if platform not in SUPPORTED_PLATFORMS:
        raise SystemExit(
            "Portable builds are currently supported on Linux and macOS only. "
            f"Current platform: {platform}"
        )

    helper_paths = {
        "dolphin-tool": resolve_helper("dolphin-tool", args.dolphin_tool),
        "wit": resolve_helper("wit", args.wit),
    }

    command = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--clean",
        "--onefile",
        "--name",
        args.name,
        "--distpath",
        str(args.dist_dir),
        "--workpath",
        str(args.work_dir),
        "--specpath",
        str(args.spec_dir),
    ]

    if args.contents_dir:
        command.extend(["--contents-directory", args.contents_dir])

    for path in helper_paths.values():
        command.extend(["--add-binary", f"{path}{separator()}tools"])

    command.append(str(ENTRYPOINT))

    print("Building portable executable with bundled helpers:")
    for name, path in helper_paths.items():
        print(f"  {name}: {path}")
    print(f"  output: {args.dist_dir / args.name}")

    subprocess.run(command, cwd=REPO_ROOT, check=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a portable onefile executable for Linux or macOS."
    )
    parser.add_argument(
        "--name",
        default="wii-rip",
        help="Output executable name (default: wii-rip)",
    )
    parser.add_argument(
        "--dist-dir",
        type=Path,
        default=REPO_ROOT / "dist",
        help="PyInstaller dist directory (default: ./dist)",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=REPO_ROOT / "build" / "pyinstaller",
        help="PyInstaller work directory (default: ./build/pyinstaller)",
    )
    parser.add_argument(
        "--spec-dir",
        type=Path,
        default=REPO_ROOT / "build" / "spec",
        help="PyInstaller spec output directory (default: ./build/spec)",
    )
    parser.add_argument(
        "--contents-dir",
        default=".",
        help=(
            "PyInstaller contents directory name for macOS app bundles. "
            "Ignored for Linux onefile builds."
        ),
    )
    parser.add_argument(
        "--dolphin-tool",
        type=Path,
        help="Explicit path to the dolphin-tool binary to bundle",
    )
    parser.add_argument(
        "--wit",
        type=Path,
        help="Explicit path to the wit binary to bundle",
    )
    return parser.parse_args()


def resolve_helper(name: str, explicit_path: Path | None) -> Path:
    if explicit_path is not None:
        return validate_helper(name, explicit_path)

    discovered = shutil.which(name)
    if not discovered:
        raise SystemExit(
            f"Could not find '{name}' in PATH. Pass --{name} with an explicit binary path."
        )
    return validate_helper(name, Path(discovered))


def validate_helper(name: str, path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise SystemExit(
            f"Expected '{name}' helper at {resolved}, but no file exists there."
        )
    return resolved


def separator() -> str:
    return ";" if sys.platform == "win32" else ":"


if __name__ == "__main__":
    main()
