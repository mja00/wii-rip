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

`wii-banner-render` is a separate C++ helper built from a vendored fork of
the [wii-banner-player](https://github.com/jordan-woyak/wii-banner-player)
source. The sources live in-tree at `Source/wii-banner-render/` (no clone or
download at build time); see `Source/wii-banner-render/NOTICE.md` for upstream
attribution and the list of in-repo modifications.

**Build dependencies (Ubuntu/Debian):**

```bash
sudo apt-get install build-essential cmake ninja-build libegl-dev
```

**Build dependencies (macOS):**

```bash
brew install cmake ninja
```

**Build:**

```bash
./packaging/build_banner_render.sh
# binary written to: build/wii-banner-render/wii-banner-render
```

**Runtime:** `wii-banner-render` requires `ffmpeg` at runtime for video
encoding (`brew install ffmpeg` on macOS, `apt-get install ffmpeg` on Linux).
On Linux, Mesa's software rasterizer (`libgl1-mesa-dri`) handles headless
OpenGL on machines without a GPU; this is typically installed by default on
Ubuntu. On macOS, the helper uses Apple's `OpenGL.framework` (legacy 2.1
profile via CGL plus an EXT_framebuffer_object FBO) for headless rendering —
no GPU server required.

**Banner text rendering:** Wii channel banners reference Wii system fonts
(BRFNT) by name. The Wii system font archive (`00000003.app`) is copyrighted
and not shipped with this project; without it, the upstream wii-banner-player
text path produces empty quads. wii-banner-render works around this by
shipping a vendored copy of [Roboto Regular](https://github.com/googlefonts/roboto)
(Apache 2.0) and rasterizing ASCII glyphs at startup using
[stb_truetype.h](https://github.com/nothings/stb), so banner titles render
out of the box even without a Wii NAND dump on disk. If you do happen to
have an extracted `00000003.app`, pass its path via
`--font-archive /path/to/00000003.app` to use the real Wii system fonts
instead.

## Building wit

On Ubuntu, `wit` is available from the system package archive (`apt-get
install wit`) and no source build is required. On macOS, install
[Homebrew](https://brew.sh/) and then build `wit` from Wiimms' sources:

```bash
brew install cmake gawk          # gawk is required by wit's setup.sh
./packaging/build_wit.sh
# binary written to: build/wit/wit
```

The script clones
[Wiimm/wiimms-iso-tools](https://github.com/Wiimm/wiimms-iso-tools), runs its
`Makefile`, and works around two macOS-specific quirks automatically:

- macOS ships BSD awk, which lacks gawk's `gensub()` extension used by wit's
  `setup.sh`. The script prepends Homebrew's gawk to `PATH` before invoking
  make so that `SYSTEM := mac` is detected correctly.
- Apple Silicon's modern linker rejects a misaligned atom in
  `dclib-numeric.o`. The script passes `XFLAGS=-Wl,-ld_classic` to fall back
  to the classic linker (tolerated by current Xcode releases).

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
  wii-rip-linux-x86_64/            (or wii-rip-macos-arm64/ on macOS)
    wii-rip
    tools/
      dolphin-tool
      wit
      wii-banner-render   (when --banner-render is passed)
```

The default package name is derived from `uname -s`/`uname -m`. On Linux it
looks like `wii-rip-linux-x86_64`; on macOS it looks like
`wii-rip-macos-arm64` or `wii-rip-macos-x86_64`.

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

The release workflow lives at `.github/workflows/release.yml` and runs two
jobs in parallel:

- `ubuntu-release` — runs on `ubuntu-latest`, source-builds `dolphin-tool`
  and `wii-banner-render`, installs `wit` from the Ubuntu package archive,
  and produces `dist/wii-rip-linux-x86_64.tar.gz`.
- `macos-release` — runs on `macos-latest` (Apple Silicon), source-builds
  `dolphin-tool`, `wit`, and `wii-banner-render`, and produces
  `dist/wii-rip-macos-arm64.tar.gz`. `--video` / `--video-only` are
  supported on both platforms.

Pull requests and pushes to `main` build and validate both artifacts. Tags
matching `v*` additionally publish both tarballs to the GitHub Release.

Each helper binary is cached on a monthly key per OS so the first build
dominates and subsequent runs reuse the cached binaries.

## Gitea Actions Releases

A matching Gitea workflow lives at `.gitea/workflows/release.yml` with the
same two jobs.

- Pull requests and pushes to `main` run the same lint, test, and package
  flow for both platforms.
- Those runs upload `wii-rip-linux-x86_64.tar.gz` and (when a macOS runner is
  available) `wii-rip-macos-arm64.tar.gz` as workflow artifacts.
- Tags matching `v*` publish the corresponding tarballs to the matching Gitea
  release.

The Gitea workflow expects a repository secret named `RELEASE_TOKEN` with
permission to create releases and upload release assets. Like the GitHub
workflow, it restores monthly caches for `dolphin-tool`, `wii-banner-render`,
and `wit` before falling back to a source build. The `macos-release` job
targets a self-hosted runner labelled `macos-latest`; if no such runner is
registered in your Gitea deployment the job will stay queued and can be
ignored, while `ubuntu-release` still ships the Linux bundle.

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

Pass a Wii shared font archive through to `wii-banner-render`:

```bash
./target/release/wii-rip "Game.rvz" -o output/ --video-only --font-archive /path/to/00000011.app
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

- Linux and macOS are the supported targets. The Rust binary builds with no
  code changes on both platforms; CI publishes tarballs for
  `linux-x86_64` and `macos-arm64`. Audio extraction, banner-animation video
  (`--video` / `--video-only`), and the muxed audio+video output all work
  identically on Linux and macOS.
- `dolphin-tool` and `wit` remain external dependencies that the build
  scripts source-build from upstream tarballs. `wii-banner-render`, by
  contrast, is now vendored in-tree at `Source/wii-banner-render/` — see
  `Source/wii-banner-render/NOTICE.md` for upstream attribution and the list
  of in-repo modifications.
- Releases that bundle these helpers should include the relevant GPL/zlib
  license texts and corresponding-source information for the exact bundled
  binaries. The vendored `wii-banner-render` sources at
  `Source/wii-banner-render/` already satisfy the GPL-2.0 corresponding-source
  obligation for that helper.
