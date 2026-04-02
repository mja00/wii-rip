# wii-rip

Extract Wii disc channel music from `.rvz`, `.iso`, and `.wbfs` images into a PCM WAV file.

## Runtime dependencies

`wii-rip` uses two external helper tools:

- `dolphin-tool` for `RVZ -> ISO`
- `wit` for extracting `opening.bnr`

The normal Python entrypoint will look for them in this order:

1. `WII_RIP_DOLPHIN_TOOL` / `WII_RIP_WIT`
2. `WII_RIP_TOOLS_DIR/<tool>`
3. Bundled helpers inside a frozen app (`tools/<tool>`)
4. `PATH`

## Portable Linux/macOS builds

Portable builds are produced per platform. There is no single binary that runs on both Linux and macOS.

### Build prerequisites

1. Install Python 3.
2. Install the helper tools you want to bundle for the current platform.
3. Install PyInstaller:

```bash
python3 -m pip install -r requirements-build.txt
```

### Build command

On Linux or macOS, run:

```bash
python3 packaging/build_portable.py
```

This will:

1. Find `dolphin-tool` and `wit` from `PATH` unless explicit paths are provided.
2. Bundle both helpers into a PyInstaller `--onefile` executable.
3. Write the result to `dist/wii-rip`.

You can override helper locations:

```bash
python3 packaging/build_portable.py \
  --dolphin-tool /path/to/dolphin-tool \
  --wit /path/to/wit
```

### Smoke test the built binary

```bash
./dist/wii-rip "Game.rvz" -o output/
```

## Local development usage

```bash
python3 wii_rip.py "Game.rvz" -o output/
```

## Notes

- Linux and macOS are the currently supported portable targets.
- Releases that bundle `dolphin-tool` and `wit` should include the relevant GPL license texts and corresponding-source information for the exact bundled binaries.
