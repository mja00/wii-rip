# wii-banner-render

A headless renderer for Wii disc channel banner animations. Loads an
`opening.bnr`, renders each frame of the layout animation to an offscreen
EGL OpenGL context, and pipes raw RGBA frames to `ffmpeg` to encode an MP4.

This is a fork of [wii-banner-player](https://github.com/jordan-woyak/wii-banner-player)
adapted for offline / CI use: there is no window, no audio output, and no
keyboard / mouse interaction. See [`MODIFICATIONS.md`](MODIFICATIONS.md) for
the full list of changes relative to upstream and [`LICENSE.txt`](LICENSE.txt)
for the licenses that apply to vendored source.

## Building

The build is driven by `packaging/build_banner_render.sh` at the repo root,
which downloads GLEW into the build directory and runs CMake / Ninja:

```bash
./packaging/build_banner_render.sh
# binary written to: build/wii-banner-render/wii-banner-render
```

Build dependencies on Ubuntu / Debian:

```bash
sudo apt-get install build-essential cmake ninja-build libegl-dev
```

`ffmpeg` is required at runtime for video encoding.
