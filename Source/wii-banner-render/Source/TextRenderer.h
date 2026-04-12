// TextRenderer — TTF-based fallback for Wii channel banner text.
//
// The upstream wii-banner-player rendered text by sampling cells out of a
// Wii system font (BRFNT) loaded from `00000003.app` on the user's Wii NAND
// dump. We cannot ship that font for licensing reasons, so this module
// provides a fallback that rasterizes ASCII glyphs from a vendored TTF
// (Roboto Regular, Apache 2.0) using stb_truetype, packs them into a single
// alpha-only OpenGL texture at startup, and draws strings as quads sampled
// from that atlas.
//
// This is NOT used when the user supplies a real Wii font archive via
// `--font-archive` (handled by the upstream Font / Banner code path).

#ifndef WII_BANNER_RENDER_TEXTRENDERER_H_
#define WII_BANNER_RENDER_TEXTRENDERER_H_

#include "Types.h"

namespace WiiBanner
{

class TextRenderer
{
public:
    // Returns the process-wide singleton, initializing the atlas on first
    // call. Returns nullptr if initialization fails.
    static TextRenderer* GetOrInit();

	// Draws `text` starting at the current modelview origin. The `target_w`
	// and `target_h` parameters are the textbox bounding box in layout
	// units; we use them to choose a font size and to clip text that
	// overflows. `text_position` is the NW4R 3x3 text-block anchor and
	// `text_alignment` is the per-line horizontal alignment override.
	// `r/g/b/a` are the foreground RGBA values (0-255).
	//
	// Modelview matrix is preserved; the function emits its own
	// glPushMatrix/glPopMatrix wrapper.
	void DrawString(const std::wstring& text,
					float target_w, float target_h,
					u8 text_position, u8 text_alignment,
					float line_spacing,
					u8 r, u8 g, u8 b, u8 a) const;

private:
    TextRenderer();
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    bool Init();

    struct Glyph
    {
        // Texture-space coordinates of the glyph cell, in [0, 1].
        float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
        // Glyph metrics in pixels at the baked size.
        float x_offset = 0.f;
        float y_offset = 0.f;
        float width    = 0.f;
        float height   = 0.f;
        float advance  = 0.f;
    };

    // ASCII range we bake. 32 (' ') through 126 ('~') is enough for the
    // Latin text in standard channel banners; uncovered codepoints fall
    // back to '?'.
    static constexpr int kFirstChar = 32;
    static constexpr int kCharCount = 95;

    // Font height (in pixels) at which the atlas was baked. Glyph quads
    // are scaled at draw time to match the textbox bounding box height.
    static constexpr float kBakedPixelHeight = 64.f;

    Glyph glyphs_[kCharCount];
    unsigned int texture_id_ = 0;
    int          atlas_width_  = 0;
    int          atlas_height_ = 0;
    float        ascent_       = 0.f;   // pixels above the baseline
    float        descent_      = 0.f;   // pixels below the baseline (negative)
    float        line_gap_     = 0.f;
    bool         initialized_  = false;
};

}  // namespace WiiBanner

#endif  // WII_BANNER_RENDER_TEXTRENDERER_H_
