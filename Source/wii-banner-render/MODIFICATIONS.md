# Modifications

This directory is a vendored, modified copy of
[wii-banner-player](https://github.com/jordan-woyak/wii-banner-player) at
upstream commit `6458757` ("boost_foreach"). The zlib license under which the
upstream is distributed permits modification and redistribution provided
altered versions are plainly marked as such — this file, together with the
in-file `// NOTE: Altered source version of ...` comments, satisfies that
requirement.

## What changed relative to upstream

| File | Change |
| --- | --- |
| `CMakeLists.txt` | Rewritten to drop SFML; build a static GLEW (compiled with `GLEW_EGL` so `glewInit()` works in a headless EGL context); link only the sources actually needed by the headless renderer; force-include `<cmath>` for newer GCC. |
| `Source/Main.cpp` | Replaced the interactive SFML windowed banner viewer with a headless EGL offscreen renderer that steps the animation frame-by-frame, captures each frame via `glReadPixels`, and pipes raw RGBA frames to `ffmpeg` for video encoding. Adds CLI flags for output path, aspect (4:3 / 16:9), fps, loop count, ffmpeg path, and language. |
| `Source/Layout.h` | Added `GetLoopStart()` and `GetLoopEnd()` public getters so the renderer can compute the total frame count for a given loop multiplier. |
| `Source/Sound.cpp` | Replaced the SFML-backed BNS audio playback with a no-op stub. wii-rip handles disc channel audio separately via its own BNS decoder, so the renderer does not need audio output. |

All other files under `Source/` and `Externals/dolphin-emu/Source/` are
unchanged from upstream.

## Files removed relative to upstream

The following parts of the upstream tree are not vendored because the headless
renderer does not use them:

- `Externals/sfml/` — SFML is no longer a dependency.
- `Externals/GLEW/` — replaced by a build-time GLEW download (see
  `packaging/build_banner_render.sh`) so we can compile with `GLEW_EGL`.
- `wii-banner-player.{cbp,sln,vcxproj}` — Code::Blocks / Visual Studio project
  files for the original SFML-based viewer.
