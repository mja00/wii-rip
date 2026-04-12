// TextRenderer — TTF-based fallback for Wii channel banner text.
// See TextRenderer.h for the design rationale.

#include "TextRenderer.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <GL/glew.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "../Externals/font/roboto_regular_ttf.h"

namespace WiiBanner
{

namespace
{

// Atlas geometry: a 16x6 grid is enough for the 95 baked glyphs (the bottom
// row has only 15 cells filled). Each cell is wide enough to hold the widest
// glyph at the baked pixel height plus a small padding.
constexpr int kCellsPerRow  = 16;
constexpr int kCellRowCount = 6;     // 16*6 = 96 >= 95 glyph slots
constexpr int kCellPadding  = 2;     // pixels between glyphs

int ResolveHorizontalAnchor(u8 text_position, u8 text_alignment)
{
	const int anchor_x = std::min<int>(text_position % 3, 2);
	switch (text_alignment & 0x7)
	{
	case 1:
		return 0;
	case 3:
		return 2;
	case 0:
		return anchor_x;
	case 2:
	default:
		return 1;
	}
}

int ResolveVerticalAnchor(u8 text_position)
{
	return std::min<int>(text_position / 3, 2);
}

float ResolveAlignedOffset(float block_width, float line_width, int align_x)
{
	switch (align_x)
	{
	case 0:
		return 0.f;
	case 2:
		return block_width - line_width;
	case 1:
	default:
		return (block_width - line_width) * 0.5f;
	}
}

struct WrappedLine
{
	std::wstring text;
	float width = 0.f;
	WrappedLine() = default;
	WrappedLine(std::wstring line_text, float line_width)
		: text(std::move(line_text)), width(line_width)
	{
	}
};

template <typename MeasureGlyph>
std::vector<WrappedLine> WrapLines(const std::wstring& text,
	float max_width, MeasureGlyph measure_glyph)
{
	std::vector<WrappedLine> lines;
	std::wstring current_line;
	float current_width = 0.f;

	auto push_line = [&]()
	{
		lines.emplace_back(current_line, current_width);
		current_line.clear();
		current_width = 0.f;
	};

	for (wchar_t wc : text)
	{
		if (wc == L'\r')
			continue;

		if (wc == L'\n')
		{
			push_line();
			continue;
		}

		const float glyph_width = measure_glyph(wc);
		const float tentative_width = current_line.empty()
			? glyph_width
			: current_width + glyph_width;

		if (max_width > 0.f && !current_line.empty() && tentative_width > max_width)
			push_line();

		current_width = current_line.empty()
			? glyph_width
			: current_width + glyph_width;
		current_line += wc;
	}

	push_line();
	return lines;
}

}  // namespace

TextRenderer* TextRenderer::GetOrInit()
{
    static TextRenderer* instance = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        auto* candidate = new TextRenderer();
        if (candidate->Init())
        {
            instance = candidate;
        }
        else
        {
            std::cerr << "wii-banner-render: TextRenderer initialization failed; "
                         "text will not be rendered\n";
            delete candidate;
        }
    }
    return instance;
}

TextRenderer::TextRenderer() = default;

TextRenderer::~TextRenderer()
{
    if (texture_id_)
    {
        const GLuint id = static_cast<GLuint>(texture_id_);
        glDeleteTextures(1, &id);
    }
}

bool TextRenderer::Init()
{
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, wii_banner_render::kRobotoRegularTtf, 0))
    {
        std::cerr << "wii-banner-render: stbtt_InitFont failed\n";
        return false;
    }

    const float scale = stbtt_ScaleForPixelHeight(&font, kBakedPixelHeight);

    int ascent_i = 0, descent_i = 0, line_gap_i = 0;
    stbtt_GetFontVMetrics(&font, &ascent_i, &descent_i, &line_gap_i);
    ascent_   = ascent_i  * scale;
    descent_  = descent_i * scale;
    line_gap_ = line_gap_i * scale;

    // First pass: rasterize each glyph into a temporary buffer and record
    // its width/height/offset/advance. We'll lay them out into the atlas
    // once we know all the cell sizes.
    struct StagedGlyph
    {
        std::vector<unsigned char> bitmap;
        int w = 0, h = 0;
        int x_off = 0, y_off = 0;
        int advance = 0;
        int lsb = 0;
    };
    std::vector<StagedGlyph> staged(kCharCount);

    int max_w = 0;
    int max_h = 0;
    for (int i = 0; i < kCharCount; ++i)
    {
        const int codepoint = kFirstChar + i;
        StagedGlyph& g = staged[i];

        unsigned char* bmp = stbtt_GetCodepointBitmap(
            &font, 0.0f, scale, codepoint, &g.w, &g.h, &g.x_off, &g.y_off);
        if (bmp)
        {
            g.bitmap.assign(bmp, bmp + (g.w * g.h));
            stbtt_FreeBitmap(bmp, nullptr);
        }
        else
        {
            // Glyphs without bitmap data (e.g., space) still need metrics.
            g.bitmap.clear();
        }

        stbtt_GetCodepointHMetrics(&font, codepoint, &g.advance, &g.lsb);

        max_w = std::max(max_w, g.w);
        max_h = std::max(max_h, g.h);
    }

    const int cell_w = max_w + kCellPadding * 2;
    const int cell_h = max_h + kCellPadding * 2;

    atlas_width_  = cell_w * kCellsPerRow;
    atlas_height_ = cell_h * kCellRowCount;

    std::vector<unsigned char> atlas(atlas_width_ * atlas_height_, 0);

    // Second pass: blit each rasterized glyph into its atlas cell and
    // record the resulting UVs / metrics.
    for (int i = 0; i < kCharCount; ++i)
    {
        const StagedGlyph& g = staged[i];
        const int col = i % kCellsPerRow;
        const int row = i / kCellsPerRow;

        const int cell_x = col * cell_w + kCellPadding;
        const int cell_y = row * cell_h + kCellPadding;

        for (int y = 0; y < g.h; ++y)
        {
            const unsigned char* src = g.bitmap.data() + y * g.w;
            unsigned char* dst = atlas.data() + (cell_y + y) * atlas_width_ + cell_x;
            std::memcpy(dst, src, g.w);
        }

        Glyph& out = glyphs_[i];
        out.u0 = static_cast<float>(cell_x) / atlas_width_;
        out.v0 = static_cast<float>(cell_y) / atlas_height_;
        out.u1 = static_cast<float>(cell_x + g.w) / atlas_width_;
        out.v1 = static_cast<float>(cell_y + g.h) / atlas_height_;

        out.x_offset = static_cast<float>(g.x_off);
        out.y_offset = static_cast<float>(g.y_off);
        out.width    = static_cast<float>(g.w);
        out.height   = static_cast<float>(g.h);
        out.advance  = g.advance * scale;
    }

    // Apple's legacy 2.1 GL implementation does not always sample
    // GL_ALPHA-format textures the way we want, so expand the alpha-only
    // bitmap to a full RGBA texture where RGB is fixed at 255 and A is
    // the rasterized coverage. With glTexEnvi(GL_MODULATE) and a vertex
    // color of (r, g, b, a), the resulting fragment is
    // (r, g, b, a * coverage / 255), which is exactly what we want for
    // anti-aliased text overlay.
    std::vector<unsigned char> rgba_atlas(atlas.size() * 4);
    for (std::size_t i = 0; i < atlas.size(); ++i)
    {
        rgba_atlas[i * 4 + 0] = 255;
        rgba_atlas[i * 4 + 1] = 255;
        rgba_atlas[i * 4 + 2] = 255;
        rgba_atlas[i * 4 + 3] = atlas[i];
    }

    GLuint id = 0;
    glGenTextures(1, &id);
    if (!id)
    {
        std::cerr << "wii-banner-render: glGenTextures failed for text atlas\n";
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas_width_, atlas_height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba_atlas.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    texture_id_  = id;
    initialized_ = true;
    return true;
}

void TextRenderer::DrawString(const std::wstring& text,
                              float target_w, float target_h,
                              u8 text_position, u8 text_alignment,
                              float line_spacing,
                              u8 r, u8 g, u8 b, u8 a) const
{
    if (!initialized_ || text.empty() || target_h <= 0.f)
        return;

    const int text_anchor_x = std::min<int>(text_position % 3, 2);

    // Pick a font size that fits the textbox vertically. The atlas was
    // baked at kBakedPixelHeight; everything else scales linearly.
    const float base_line_height = ascent_ - descent_;
    const float base_line_step = base_line_height + line_gap_;
    float scale = 1.f;

    auto measure_glyph = [&](wchar_t c)
    {
        int idx = static_cast<int>(c) - kFirstChar;
        if (idx < 0 || idx >= kCharCount)
            idx = '?' - kFirstChar;
        return glyphs_[idx].advance;
    };

    std::vector<WrappedLine> lines;
    for (int pass = 0; pass < 2; ++pass)
    {
        const float max_unscaled_width = scale > 0.f ? target_w / scale : target_w;
        lines = WrapLines(text, max_unscaled_width, measure_glyph);

        const float scalable_text_h = base_line_height
            + (lines.size() > 1 ? (lines.size() - 1) * base_line_step : 0.f);
        const float fixed_line_spacing =
            lines.size() > 1 ? (lines.size() - 1) * line_spacing : 0.f;
        if (scalable_text_h > 0.f)
        {
            const float available_h = std::max(0.f, target_h - fixed_line_spacing);
            scale = available_h / scalable_text_h;
        }
    }

    // Measure the unscaled width of each line so we can horizontally
    // center it inside the textbox bounding box.
    std::vector<float> line_advances;
    line_advances.reserve(lines.size());
    float max_line_advance = 0.f;
    for (const WrappedLine& line : lines)
    {
        const float line_advance = line.width;
        line_advances.push_back(line_advance);
        max_line_advance = std::max(max_line_advance, line_advance);
    }

    // If any line would overflow the textbox horizontally, shrink the
    // scale until the widest line fits.
    if (max_line_advance * scale > target_w && max_line_advance > 0.f)
        scale = target_w / max_line_advance;

    const float block_width = max_line_advance * scale;
    const float block_left = ResolveAlignedOffset(target_w, block_width, text_anchor_x);

    glPushAttrib(GL_ENABLE_BIT | GL_TEXTURE_BIT | GL_CURRENT_BIT
                 | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT
                 | GL_STENCIL_BUFFER_BIT);

    // The Wii banner material renderer (WrapGx) installs a GLSL program
    // that translates GX TEV stages into fragment shader logic. That
    // program ignores both glColor and the texture we bind here, so
    // disable it for the duration of the text draw and switch back to the
    // fixed-function pipeline. We restore the previously bound program
    // afterwards so subsequent panes keep working.
    GLint previous_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
    glUseProgram(0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Disable texturing on every other unit so the FFP only samples our
    // atlas. Apple's legacy GL driver, like Mesa, will multiply across all
    // enabled texture units; the WrapGx material setup leaves textures
    // bound and enabled on units 1+.
    for (int u = 7; u > 0; --u)
    {
        glActiveTexture(GL_TEXTURE0 + u);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_id_));
    glEnable(GL_TEXTURE_2D);

    // Use the fixed-function texture environment to multiply the atlas
    // alpha by the vertex color: produces (r, g, b, a * atlas_alpha / 255).
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // The Wii banner pipeline (WrapGx + Material) leaves vertex array
    // client state enabled with pointers into Pane / Material vertex data.
    // glBegin/glEnd in legacy GL still respect those client arrays for any
    // attribute that has its array enabled, which means our immediate-mode
    // vertices would be overridden by stale array bindings. Disable every
    // client array so the FFP only uses what we emit between glBegin/glEnd.
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_INDEX_ARRAY);
    glDisableClientState(GL_EDGE_FLAG_ARRAY);

    // Drain any GL error left over from earlier banner draws so a future
    // diagnostic glGetError() in this function doesn't blame us for it.
    while (glGetError() != GL_NO_ERROR) {}

    glColor4ub(r, g, b, a);

    glPushMatrix();

    // The Wii layout passes us a textbox-local frame where the parent
    // pane chain has already flipped Y relative to our (+X right, +Y down)
    // assumption. Without correction, glyphs come out vertically mirrored.
    // Mirror around the textbox center to put +Y back the way TextRenderer
    // expects.
    glTranslatef(0.f, target_h * 0.5f, 0.f);
    glScalef(1.f, -1.f, 1.f);
    glTranslatef(0.f, -target_h * 0.5f, 0.f);

    // Vertically place the first baseline so the full multi-line block
    // fits symmetrically within the textbox.
    const float scaled_ascent = ascent_ * scale;
    const float scaled_descent = descent_ * scale;       // negative
    const float scaled_line_height = scaled_ascent - scaled_descent;
    const float scaled_line_step = base_line_step * scale + line_spacing;
    const float scaled_text_height = scaled_line_height
        + (lines.size() > 1
            ? (lines.size() - 1) * scaled_line_step
            : 0.f);
    float first_baseline_y = scaled_ascent;
    switch (ResolveVerticalAnchor(text_position))
    {
    case 0:
        break;
    case 2:
        first_baseline_y = target_h - scaled_text_height + scaled_ascent;
        break;
    case 1:
    default:
        first_baseline_y = (target_h - scaled_text_height) * 0.5f + scaled_ascent;
        break;
    }

    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index)
    {
	    const float x_origin = block_left + ResolveAlignedOffset(
	        block_width,
	        line_advances[line_index] * scale,
	        ResolveHorizontalAnchor(text_position, text_alignment));
        const float baseline_y =
            first_baseline_y + static_cast<float>(line_index) * scaled_line_step;

        float pen_x = 0.f;
        for (wchar_t c : lines[line_index].text)
        {
            int idx = static_cast<int>(c) - kFirstChar;
            if (idx < 0 || idx >= kCharCount)
                idx = '?' - kFirstChar;
            const Glyph& gl = glyphs_[idx];

            const float x0 = x_origin + pen_x + gl.x_offset * scale;
            const float y0 = baseline_y + gl.y_offset * scale;
            const float x1 = x0 + gl.width  * scale;
            const float y1 = y0 + gl.height * scale;

            glBegin(GL_QUADS);
            glTexCoord2f(gl.u0, gl.v0); glVertex2f(x0, y0);
            glTexCoord2f(gl.u1, gl.v0); glVertex2f(x1, y0);
            glTexCoord2f(gl.u1, gl.v1); glVertex2f(x1, y1);
            glTexCoord2f(gl.u0, gl.v1); glVertex2f(x0, y1);
            glEnd();

            pen_x += gl.advance * scale;
        }
    }
    glPopMatrix();

    // Restore the GLSL program WrapGx had bound before our text overlay,
    // so subsequent material/textbox draws still see their own shader.
    glUseProgram(static_cast<GLuint>(previous_program));

    glPopAttrib();
}

}  // namespace WiiBanner
