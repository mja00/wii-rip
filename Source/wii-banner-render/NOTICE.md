# wii-banner-render — Vendored Sources Notice

This directory contains a fork of [wii-banner-player](https://github.com/jordan-woyak/wii-banner-player) — the NW4R layout / Wii channel banner renderer by Jordan Woyak — alongside the small slice of [Dolphin Emulator](https://github.com/dolphin-emu/dolphin) that wii-banner-player depends on, a vendored copy of [GLEW 2.2.0](https://github.com/nigels-com/glew), the [stb_truetype.h](https://github.com/nothings/stb) single-header library, and a [Roboto Regular](https://github.com/googlefonts/roboto) TTF used for fallback text rendering. The sources are committed in-tree, modified in place, and built into the helper binary `wii-banner-render` that wii-rip drives via `--video` / `--video-only`.

The motivation for vendoring (rather than cloning the upstream repos at build time) is to:
- Ship one self-contained, hermetic build
- Make every modification visible in version control
- Comply with GPL-2.0 "corresponding source" requirements for the Dolphin slice
- Stop relying on third-party repos staying online and reachable from CI

## Provenance

| Subtree | Upstream | Version | License |
|---|---|---|---|
| `Source/` (banner player files) | https://github.com/jordan-woyak/wii-banner-player | `master` (2010-2012 era) | zlib (see `Source/<file>.cpp` headers) |
| `Source/TextRenderer.{h,cpp}` | this project (new) | n/a | zlib (matches the surrounding wii-banner-player files) |
| `Externals/dolphin-emu/Source/` | https://github.com/dolphin-emu/dolphin (snapshot vendored by wii-banner-player) | matches wii-banner-player's snapshot | GPL-2.0 (see file headers) |
| `Externals/glew/` | https://github.com/nigels-com/glew/releases/tag/glew-2.2.0 | 2.2.0 | Modified BSD / MIT / Khronos (see `glew.h` header preamble) |
| `Externals/stb/stb_truetype.h` | https://github.com/nothings/stb | v1.26 | public domain (see file header) |
| `Externals/font/Roboto-Regular.ttf` and `roboto_regular_ttf.h` | https://github.com/googlefonts/roboto | hinted/Roboto-Regular.ttf | Apache 2.0 (see `Externals/font/LICENSE.Roboto`) |

## Modifications

All modifications are restricted to making the upstream code compile on Apple Silicon macOS. The Linux x86_64 build path is functionally unchanged. Each touched file carries a `NOTE: ... Modifications:` block at the top — search for `NOTE: Modified` or `NOTE: Altered source version` to find them. The summary:

### `Source/Main.cpp` (zlib, altered)
Replaces wii-banner-player's interactive SFML windowed viewer with a headless renderer that creates an offscreen GL context, steps through the banner animation frame-by-frame, and pipes raw RGBA frames to `ffmpeg` for H.264 encoding. On Linux it uses an EGL pbuffer surface; on macOS it uses CGL (Apple's native low-level GL API, part of `OpenGL.framework`) configured for the legacy 2.1 profile, plus an `EXT_framebuffer_object` FBO with a packed `DEPTH24_STENCIL8` renderbuffer as the offscreen render target.

### `Source/Layout.h` (zlib, altered)
Two changes:
1. Added public `GetLoopStart()` / `GetLoopEnd()` accessors so `Main.cpp` can compute frame counts for the animation loop.
2. Added an explicit `#include <list>`. The upstream header relied on `Font.h` pulling it in transitively, but the new `Font.h` no longer uses `std::list`, so `Layout.h` now owns its own include.

### `Source/Sound.cpp` (zlib, altered)
Replaced the SFML-based BNS audio player with a stub. Audio is intentionally omitted from this binary — `wii-rip` extracts disc channel audio via its own BNS decoder.

### `Source/Funcs.h` (zlib, altered)
Removed `#include <boost/foreach.hpp>` and rewrote the `foreach(decl, container)` macro to expand to a C++11 range-based for loop. The upstream comment already noted that `BOOST_FOREACH` should be replaced; this drops the Boost build dependency on every platform. Every call site uses the simple `foreach(decl, container)` form, which maps cleanly.

### `Source/WrapGx.cpp` (zlib, altered)
Removed `#include <GL/glu.h>` and `#include <GL/gl.h>`. The file calls no GLU function, and `<GL/glew.h>` (already included on the previous line) provides every GL declaration the file uses. `<GL/glu.h>` is missing in modern macOS SDKs and `<GL/gl.h>` lives at `<OpenGL/gl.h>` on Apple — dropping both stale includes lets the file build cleanly on Apple Silicon.

### `Source/Banner.h` and `Source/Banner.cpp` (zlib, altered)
Added an optional `font_archive` parameter to the `Banner` constructor and threaded it through to `LoadLayout`. The original `LoadLayout` opened the hardcoded path `"00000003.app"` (the Wii system font archive that lives at `/title/00000001/00000002/content/00000003.app` inside an extracted Wii NAND, copyrighted and not shipped with this project). After the change, `LoadLayout` first tries to resolve each font from inside the channel's own U8 archive (channel banners usually embed their own `arc/font/<name>.brfnt`) and only falls back to the external `font_archive` path for fonts that the channel doesn't provide. When both sources miss, `Textbox::Draw` falls back to the bundled Roboto TTF via `TextRenderer`. `Font::Load` is also tolerant of read errors spilling past the BRFNT section, so any failbit/eofbit state the loader leaves behind on the shared inner-archive stream is cleared before the subsequent brlan animation seeks run.

### `Source/Font.h` and `Source/Font.cpp` (zlib, altered)
The upstream BRFNT loader has been rewritten to actually understand the format it claims to load. The changes fall into four groups:

1. **Header magic**: accept both `RFNT` (the channel-embedded font magic, e.g. `arc/font/font.brfnt` inside a banner's U8 archive) and `RFNA` (the Revolution-era shared-font magic used by `wbf1.brfna` / `wbf2.brfna` inside the Wii NAND at `00000011.app`). Upstream only accepted `RFNA` and silently rejected every channel-embedded font as a bad header.
2. **TGLP section**: removed the duplicate-read off-by-one that shifted every later field by two bytes, so `cell_width`, `cell_height`, `sheet_row`, `sheet_line`, `sheet_width`, `sheet_height`, and `sheet_image` all land in their correct slots. `sheet_image` is now read as big-endian u32 (matching the rest of the BRFNT layout) and seeked relative to the file start rather than the enclosing stream; the upstream LE-plus-absolute path silently corrupted every loaded texture sheet. Every sheet in a multi-sheet BRFNT is now decoded separately via `TexDecoder_Decode(..., rgbaOnly=true)` (so I4/I8/IA4/IA8 intensity ends up replicated into all four RGBA channels, doubling as an alpha mask when modulated by `glColor`) and uploaded as its own plain `GL_TEXTURE_2D`. `Textbox::Draw` picks the correct sheet per glyph at draw time. Upstream loaded only the first sheet and dropped the rest, which limited fonts to their first ~200 glyphs.
3. **CMAP linked list**: the parser now walks every CMAP sub-section and populates a `char_code -> glyph_index` `std::map`, with support for all three NW4R mapping methods (0 = direct, 1 = table, 2 = scan). Upstream stored only the starting codepoint of the first CMAP entry, which is why text only rendered for ASCII-range linear fonts and broke as soon as a channel author used a sparse CMAP.
4. **CWDH linked list**: the parser now walks every CWDH sub-section and merges the per-glyph `left` / `glyph_width` / `char_width` metrics into a single dense vector keyed by glyph index. `Textbox::Draw` uses these for per-character advances and left-bearing offsets instead of hard-advancing by cell_width. A sensible default (from the FINF `default_width` fields) is used for glyphs outside the parsed range.

`Font::Apply()` is now a no-op: the BRFNT rendering path in `Textbox::Draw` manages its own GL state and binds sheets on demand via `Font::BindSheet()`, bypassing WrapGx.cpp's tev-compiled shader pipeline entirely. The glyph-sheet metadata (`cell_width`, `cell_height`, `sheet_row`, `sheet_line`, `sheet_width`, `sheet_height`, `sheet_format`, `glyphs_per_sheet`, plus the FINF-derived `linefeed`, `alter_char_index`, `default_char_width`, `font_height`, `font_width`, `font_ascent`) is exposed as public fields so `Textbox::Draw` can drive layout, scaling, and alignment from it. The "testing" line `sheet_height = sheet_line * cell_height;` — which clobbered the value just read from the file — is gone.

### `Source/Textbox.cpp` (zlib, altered)
Two fixes to make banner text actually render:
1. The original render loop emitted four hardcoded texture coordinates `(0,0) - (1/27, 1/128)` for every glyph, so every text box was drawn as a uniform black bar (sampling the same top-left atlas cell). The loop has been rewritten and now has two code paths. When a real BRFNT font is loaded (either from the channel's own archive or from `--font-archive`), it looks each character up via `Font::GetGlyphIndex` (which consults the parsed CMAP chain), computes the sheet / column / row from the font's TGLP metrics, advances the pen by the CWDH `char_width` for that glyph, and scales from the font's native cell dimensions to the textbox's own `width` / `height`. It does this with the fixed-function pipeline (`glUseProgram(0)`, `GL_MODULATE`, immediate-mode quads) so it bypasses WrapGx.cpp's tev-compiled shader, which is set up for textures but not for the font's alpha-replicated-into-RGBA layout. When no usable BRFNT is present, it falls back to `TextRenderer`, which rasterizes the string with `stb_truetype` against the bundled Roboto TTF.
2. The `Load()` reader for the UTF-16 string used `file >> BE >> wchar_t`, which on macOS/Linux (where `wchar_t` is 32-bit) consumed four bytes per "character" and concatenated consecutive UTF-16 pairs into garbage codepoints. The loader now reads each character as `u16` and widens it to `wchar_t`, which works on every host.

### `Source/TextRenderer.h` and `Source/TextRenderer.cpp` (zlib, new)
New module that provides a TTF-based text fallback when no Wii system font archive is supplied. On first use it rasterizes ASCII glyphs (32-126) from the bundled Roboto Regular TTF using `stb_truetype`, packs them into a single OpenGL texture atlas, and exposes a `DrawString` API. `Textbox::Draw` calls into it whenever the BRFNT path is unavailable so banner text always renders, with or without a Wii NAND dump on disk.

### `Externals/dolphin-emu/Source/Common.h` (GPL-2.0, modified)
Restricted the `#define _M_SSE 0x301` block to actual x86 hosts. The original condition was `__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3) || defined __APPLE__`, which on Apple Silicon clang (where `__APPLE__` is still defined) incorrectly enabled SSE intrinsics on arm64. Added `(defined(__x86_64__) || defined(__i386__))` to the condition.

### `Externals/dolphin-emu/Source/CommonFuncs.h` (GPL-2.0, modified)
Two changes:
1. The SSSE3 polyfill block that includes `<emmintrin.h>` is now restricted to x86 hosts (`defined(__x86_64__) || defined(__i386__)`). Same root cause as the `Common.h` patch above.
2. The forward declarations of `strndup` and `strnlen` are now skipped on Apple. macOS already provides both functions in `<string.h>` with C linkage; re-declaring them in C++ scope produced "different language linkage" errors. Linux still gets the original behaviour.

### `Externals/glew/include/GL/glew.h` (BSD, modified)
The `typedef unsigned int GLhandleARB;` declaration is now wrapped in `#ifdef __APPLE__` so that on Apple platforms it expands to `typedef void *GLhandleARB;`, matching the definition in Apple's `<OpenGL/gltypes.h>`. The original (unguarded) typedef produced a "typedef redefinition with different types" error when `glew.h` was included alongside Apple's OpenGL framework.

## Files NOT in this tree

This vendored snapshot only contains the slice of upstream needed to compile `wii-banner-render`. The wii-banner-player editor UI, SFML audio backend, font tooling, and Win32 build files are not included. The Dolphin snapshot is similarly trimmed to the few files (`Blob`, `FileBlob`, `FileHandlerARC`, `TextureDecoder`, plus their transitive headers) that the banner code transitively pulls in. Of GLEW we only ship `include/GL/glew.h` and `src/glew.c` — the GLX/WGL/EGL extension wranglers are not needed because `Main.cpp` creates the GL context directly via EGL/CGL.
