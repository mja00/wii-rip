#!/usr/bin/env python3
"""
wii_rip.py — Extract disc channel theme music from Wii ROM files.

Supported input formats: RVZ, ISO, WBFS
Output: WAV (PCM16)

Requires:
  - dolphin-tool  (pacman: dolphin-emu-tool)  — for RVZ → ISO conversion
  - wit           (pacman: wit)               — for ISO file extraction

Usage:
  python wii_rip.py game.rvz
  python wii_rip.py game.rvz -o output/
  python wii_rip.py game.iso --keep-temp
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from lib.bnr_parser import extract_sound_bin
from lib.bns_decoder import decode_bns_to_wav


TOOL_ENV_OVERRIDES = {
    "dolphin-tool": "WII_RIP_DOLPHIN_TOOL",
    "wit": "WII_RIP_WIT",
}


def check_tool(name: str) -> Path:
    """Resolve a required helper binary from overrides, the bundle, or PATH."""
    for candidate in _tool_candidates(name):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate

    print(_missing_tool_message(name), file=sys.stderr)
    sys.exit(1)


def _tool_candidates(name: str) -> list[Path]:
    candidates: list[Path] = []

    override = os.environ.get(TOOL_ENV_OVERRIDES[name])
    if override:
        candidates.append(Path(override).expanduser())

    tools_dir = os.environ.get("WII_RIP_TOOLS_DIR")
    if tools_dir:
        candidates.append(Path(tools_dir).expanduser() / name)

    bundle_root = _bundle_root()
    candidates.extend(
        [
            bundle_root / "tools" / name,
            bundle_root / name,
        ]
    )

    path_candidate = shutil.which(name)
    if path_candidate:
        candidates.append(Path(path_candidate))

    # Keep lookup order stable while avoiding duplicate probes.
    deduped: list[Path] = []
    seen: set[Path] = set()
    for candidate in candidates:
        resolved = candidate.expanduser()
        if resolved not in seen:
            seen.add(resolved)
            deduped.append(resolved)
    return deduped


def _bundle_root() -> Path:
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", Path(sys.executable).resolve().parent))
    return Path(__file__).resolve().parent


def _missing_tool_message(tool: str) -> str:
    lines = [f"Error: required helper '{tool}' was not found."]

    override_name = TOOL_ENV_OVERRIDES[tool]
    lines.append(f"Searched override: ${override_name}")
    lines.append(
        "Searched bundle paths: ./tools/<name> and ./<name> inside the app bundle"
    )
    lines.append("Searched PATH as a fallback")

    if getattr(sys, "frozen", False):
        lines.append(
            f"This portable build expects a bundled '{tool}' binary for this platform, or you can set {override_name}."
        )
    else:
        lines.append(f"Install it with your package manager or set {override_name}.")

    package = _package_for(tool)
    if package != tool:
        lines.append(f"Example package name: {package}")

    return "\n".join(lines)


def _package_for(tool: str) -> str:
    return {"dolphin-tool": "dolphin-emu-tool", "wit": "wit"}.get(tool, tool)


def rvz_to_iso(rvz_path: Path, iso_path: Path) -> None:
    """Convert RVZ to ISO using dolphin-tool."""
    dolphin = check_tool("dolphin-tool")
    print("Converting RVZ → ISO (this may take a while)...")
    result = subprocess.run(
        [
            str(dolphin),
            "convert",
            "-f",
            "iso",
            "-i",
            str(rvz_path),
            "-o",
            str(iso_path),
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"dolphin-tool failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    print(f"  ISO written to: {iso_path}")


def extract_opening_bnr(iso_path: Path, extract_dir: Path) -> Path:
    """Extract opening.bnr from a Wii ISO using wit."""
    wit = check_tool("wit")
    print("Extracting opening.bnr from disc image...")
    result = subprocess.run(
        [
            str(wit),
            "extract",
            str(iso_path),
            "--dest",
            str(extract_dir),
            "--psel",
            "data",
            "--files",
            "+opening.bnr",
            "--overwrite",
            "--quiet",
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"wit failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)

    # wit places files under extract_dir/<GAME_ID>/files/
    matches = list(extract_dir.rglob("opening.bnr"))
    if not matches:
        print("Error: opening.bnr not found after extraction.", file=sys.stderr)
        print("This disc may not have a disc channel banner.", file=sys.stderr)
        sys.exit(1)

    bnr_path = matches[0]
    print(f"  Found: {bnr_path} ({bnr_path.stat().st_size:,} bytes)")
    return bnr_path


def rip(input_path: Path, output_dir: Path, keep_temp: bool = False) -> Path:
    """Full pipeline: disc image → WAV."""
    suffix = input_path.suffix.lower()
    output_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="wii_rip_") as tmpdir:
        tmp = Path(tmpdir)

        # Step 1: Get an ISO
        if suffix == ".rvz":
            iso_path = tmp / "game.iso"
            rvz_to_iso(input_path, iso_path)
        elif suffix in (".iso", ".wbfs"):
            iso_path = input_path
        else:
            print(
                f"Error: Unsupported format '{suffix}'. Expected .rvz, .iso, or .wbfs",
                file=sys.stderr,
            )
            sys.exit(1)

        # Step 2: Extract opening.bnr
        extract_dir = tmp / "extracted"
        bnr_path = extract_opening_bnr(iso_path, extract_dir)

        # Step 3: Parse BNR → sound.bin
        print("Parsing opening.bnr...")
        bnr_data = bnr_path.read_bytes()
        try:
            sound_bin = extract_sound_bin(bnr_data)
        except ValueError as e:
            print(f"Error parsing opening.bnr: {e}", file=sys.stderr)
            sys.exit(1)
        print(
            f"  Extracted sound.bin ({len(sound_bin):,} bytes, format: {sound_bin[:4]!r})"
        )

        # Optionally save temp files for inspection
        if keep_temp:
            kept = output_dir / "sound.bin"
            kept.write_bytes(sound_bin)
            print(f"  Saved sound.bin to: {kept}")

        # Step 4: Decode BNS → WAV
        print("Decoding audio...")
        stem = input_path.stem
        wav_path = output_dir / f"{stem}_disc_channel.wav"
        try:
            sample_rate, channels, num_samples = decode_bns_to_wav(sound_bin, wav_path)
        except ValueError as e:
            print(f"Error decoding audio: {e}", file=sys.stderr)
            sys.exit(1)

        duration = num_samples / sample_rate
        print(
            f"  {channels}ch, {sample_rate} Hz, {num_samples:,} samples ({duration:.1f}s)"
        )
        print(f"\nOutput: {wav_path}")
        return wav_path


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Extract disc channel theme music from a Wii ROM.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "input", type=Path, help="Input ROM file (.rvz, .iso, or .wbfs)"
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("output"),
        help="Output directory (default: ./output)",
    )
    parser.add_argument(
        "--keep-temp",
        action="store_true",
        help="Save intermediate files (sound.bin) alongside the WAV",
    )
    args = parser.parse_args()

    if not args.input.exists():
        print(f"Error: Input file not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    rip(args.input, args.output, keep_temp=args.keep_temp)


if __name__ == "__main__":
    main()
