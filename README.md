# wii-rip

Extract Wii disc channel music from `.rvz`, `.iso`, and `.wbfs` images into a PCM WAV file.

`wii-rip` is now a native Rust CLI. It no longer ships an embedded Python runtime.

## Runtime dependencies

`wii-rip` still relies on two external helper tools:

- `dolphin-tool` for `RVZ -> ISO`
- `wit` for extracting `opening.bnr`

The binary looks for them in this order:

1. `WII_RIP_DOLPHIN_TOOL` / `WII_RIP_WIT`
2. `WII_RIP_TOOLS_DIR/<tool>`
3. `<binary-dir>/tools/<tool>`
4. `<binary-dir>/<tool>`
5. `PATH`

## Build

```bash
cargo build --release
```

The native binary will be written to:

```text
target/release/wii-rip
```

## Package A Release Directory

Build a portable directory that bundles the Rust binary with `dolphin-tool` and `wit`:

```bash
./packaging/build_release.sh
```

This writes a directory like:

```text
dist/
  wii-rip-linux-x86_64/
    wii-rip
    tools/
      dolphin-tool
      wit
```

You can also create a tarball at the same time:

```bash
./packaging/build_release.sh --tar
```

Override helper paths if they are not on `PATH`:

```bash
./packaging/build_release.sh \
  --dolphin-tool /path/to/dolphin-tool \
  --wit /path/to/wit
```

## Usage

```bash
./target/release/wii-rip "Game.rvz" -o output/
```

Save the intermediate `sound.bin` alongside the WAV if you want to inspect it:

```bash
./target/release/wii-rip "Game.rvz" -o output/ --keep-temp
```

The staged layout matches the runtime lookup order, so the packaged binary will find its bundled helpers automatically.

## Development

```bash
cargo run -- "Game.rvz" -o output/
```

## Notes

- Linux and macOS are the supported targets.
- `dolphin-tool` and `wit` remain external dependencies; this rewrite only removes the embedded Python environment.
- Releases that bundle `dolphin-tool` and `wit` should include the relevant GPL license texts and corresponding-source information for the exact bundled binaries.
