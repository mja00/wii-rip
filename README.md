# wii-rip

Extract Wii disc channel audio and/or banner animation from `.rvz`, `.iso`, and `.wbfs` images.

`wii-rip` is a native Rust CLI. It no longer ships an embedded Python runtime.

## Runtime dependencies

`wii-rip` relies on external helper tools:

- `dolphin-tool` — required for `RVZ -> ISO` conversion
- `wit` — required for extracting `opening.bnr`
- `wii-banner-render` — required for `--video` / `--video-only` (renders the banner animation to MP4)
- `ffmpeg` — optional; muxes audio + video into a single file when both are extracted

The binary looks for each tool in this order:

1. `WII_RIP_DOLPHIN_TOOL` / `WII_RIP_WIT` / `WII_RIP_BANNER_RENDER` / `WII_RIP_FFMPEG`
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

## Building wii-banner-render

`wii-banner-render` is a separate C++ helper built from the
[wii-banner-player](https://github.com/jordan-woyak/wii-banner-player) source
with modifications for headless video export. The build script handles cloning
and patching automatically.

**Build dependencies (Ubuntu/Debian):**

```bash
sudo apt-get install build-essential cmake ninja-build libglew-dev libegl-dev
```

**Build:**

```bash
./packaging/build_banner_render.sh
# binary written to: build/wii-banner-render/wii-banner-render
```

**Runtime:** `wii-banner-render` requires `ffmpeg` at runtime for video
encoding. Mesa's software rasterizer (`libgl1-mesa-dri`) handles headless OpenGL
on machines without a GPU; this is typically installed by default on Ubuntu.

## Package A Release Directory

Build a portable directory that bundles the Rust binary with `dolphin-tool`,
`wit`, and optionally `wii-banner-render`:

```bash
# Audio-only bundle (no wii-banner-render):
./packaging/build_release.sh

# Full bundle including wii-banner-render:
./packaging/build_release.sh \
  --banner-render build/wii-banner-render/wii-banner-render
```

This writes a directory like:

```text
dist/
  wii-rip-linux-x86_64/
    wii-rip
    tools/
      dolphin-tool
      wit
      wii-banner-render   (when --banner-render is passed)
```

You can also create a tarball at the same time:

```bash
./packaging/build_release.sh --tar
```

Override helper paths if they are not on `PATH`:

```bash
./packaging/build_release.sh \
  --dolphin-tool /path/to/dolphin-tool \
  --wit /path/to/wit \
  --banner-render /path/to/wii-banner-render
```

## GitHub Actions Releases

An Ubuntu release workflow lives at `.github/workflows/release.yml`.

- Pull requests and pushes to `main` build and validate the bundled Linux release artifact.
- Tags matching `v*` also publish `dist/wii-rip-linux-x86_64.tar.gz` to the GitHub Release.

The workflow source-builds both `dolphin-tool` and `wii-banner-render`, caching
each binary on a monthly key. `wit` is installed from the Ubuntu package archive.

## Gitea Actions Releases

A matching Gitea workflow lives at `.gitea/workflows/release.yml`.

- Pull requests and pushes to `main` run the same Ubuntu build, lint, test, and package flow.
- Those runs also upload `wii-rip-linux-x86_64.tar.gz` as a workflow artifact.
- Tags matching `v*` publish `dist/wii-rip-linux-x86_64.tar.gz` to the matching Gitea release.

The Gitea workflow expects a repository secret named `RELEASE_TOKEN` with permission to create releases and upload release assets.
Like the GitHub workflow, it restores monthly caches for `dolphin-tool` and `wii-banner-render` before falling back to a source build.

## Usage

Extract disc channel audio (WAV):

```bash
./target/release/wii-rip "Game.rvz" -o output/
```

Also render the banner animation to MP4 (requires `wii-banner-render`):

```bash
./target/release/wii-rip "Game.rvz" -o output/ --video
```

Render the banner animation in 16:9 instead of the default 4:3:

```bash
./target/release/wii-rip "Game.rvz" -o output/ --video --video-aspect 16:9
```

Render both 4:3 and 16:9 variants:

```bash
./target/release/wii-rip "Game.rvz" -o output/ --video --video-aspect both
```

Render banner animation only, skip audio extraction:

```bash
./target/release/wii-rip "Game.rvz" -o output/ --video-only
```

Save the intermediate `sound.bin` alongside the WAV if you want to inspect it:

```bash
./target/release/wii-rip "Game.rvz" -o output/ --keep-temp
```

When both `--video` and audio are extracted and `ffmpeg` is available, a muxed
`<stem>_disc_channel.mp4` (audio + video) is written for the default 4:3 render.
Non-default aspect outputs are suffixed, for example `*_disc_channel_16x9.mp4` and
`*_disc_channel_banner_16x9.mp4`. With `--video-aspect both`, both suffixed variants
are written. Without `ffmpeg`, the audio WAV and banner MP4s are written separately
with suggested mux commands.

The staged layout matches the runtime lookup order, so the packaged binary will find its bundled helpers automatically.

## Development

```bash
cargo run -- "Game.rvz" -o output/
```

## Notes

- Linux and macOS are the supported targets.
- `dolphin-tool`, `wit`, and `wii-banner-render` remain external dependencies.
- Releases that bundle these helpers should include the relevant GPL/zlib license texts and corresponding-source information for the exact bundled binaries.
- `wii-banner-render` is based on the [wii-banner-player](https://github.com/jordan-woyak/wii-banner-player) project (zlib license) and uses the NW4R layout engine re-implementation from that project.
