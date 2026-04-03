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

## GitHub Actions Releases

An Ubuntu release workflow lives at `.github/workflows/release.yml`.

- Pull requests and pushes to `main` build and validate the bundled Linux release artifact.
- Tags matching `v*` also publish `dist/wii-rip-linux-x86_64.tar.gz` to the GitHub Release.

The workflow installs Ubuntu's `wit` package and source-builds `dolphin-tool` from the pinned Dolphin ref in the workflow before calling `./packaging/build_release.sh`.
It caches the built `dolphin-tool` binary on a monthly key, so the helper is usually rebuilt only once per month per pinned Dolphin ref.

## Gitea Actions Releases

A matching Gitea workflow lives at `.gitea/workflows/release.yml`.

- Pull requests and pushes to `main` run the same Ubuntu build, lint, test, and package flow.
- Tags matching `v*` publish `dist/wii-rip-linux-x86_64.tar.gz` to the matching Gitea release.

The Gitea workflow expects a repository secret named `RELEASE_TOKEN` with permission to create releases and upload release assets.
Like the GitHub workflow, it restores a monthly `dolphin-tool` cache before falling back to a source build.

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
