/*
Copyright (c) 2010 - Wii Banner Player Project

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

// NOTE: Altered source version of Textbox.cpp from the wii-banner-player project.
// Modifications:
//   - Rewrote Textbox::Load to read the on-disk UTF-16 string as a u16
//     and widen it to wchar_t explicitly.  `file >> BE >> wchar_t` is
//     wrong because wchar_t is 32-bit on macOS/Linux and only 16-bit on
//     Windows: on POSIX the upstream code consumed four bytes per glyph
//     and merged consecutive UTF-16 code units into garbage codepoints,
//     which rendered every non-first glyph as the same cell of the font
//     sheet.
//   - Rewrote the Draw() glyph loop entirely.  Upstream hardcoded texture
//     coordinates (0, 0) - (1/27, 1/128) for every glyph, always sampled
//     the top-left cell of the font sheet, and produced a uniform black
//     bar for every text box.  The new loop has two paths:
//       1. When a real BRFNT font is loaded (channel-embedded font.brfnt
//          or a Wii system font via --font-archive), it looks each
//          character up through the font's parsed CMAP, computes the
//          correct sheet / column / row from the font's TGLP metrics,
//          advances the pen by the CWDH `char_width` for that glyph, and
//          scales from the font's native cell dimensions to the textbox's
//          own font_width / font_height.  The loop bypasses WrapGx.cpp's
//          tev-compiled shader (via glUseProgram(0)) and draws the glyph
//          quad directly against the font sheet texture bound by
//          Font::BindSheet, using fixed-function GL_MODULATE for the
//          per-vertex colour * texture-alpha combination.
//       2. When no real BRFNT is usable (old-style banners without an
//          embedded font, or a --font-archive that couldn't be opened),
//          it falls back to TextRenderer, which rasterises glyphs from
//          the vendored Roboto Regular TTF so banner text still renders
//          with or without a Wii NAND dump on disk.

#include <GL/glew.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "Font.h"
#include "Textbox.h"
#include "Layout.h"
#include "Endian.h"
#include "TextRenderer.h"

namespace WiiBanner
{

namespace
{

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
	float max_width, float extra_spacing, MeasureGlyph measure_glyph)
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
			: current_width + extra_spacing + glyph_width;

		if (max_width > 0.f && !current_line.empty() && tentative_width > max_width)
			push_line();

		current_width = current_line.empty()
			? glyph_width
			: current_width + extra_spacing + glyph_width;
		current_line += wc;
	}

	push_line();
	return lines;
}

int ResolveTextAnchorX(u8 text_position)
{
	return std::min<int>(text_position % 3, 2);
}

int ResolveTextAnchorY(u8 text_position)
{
	return std::min<int>(text_position / 3, 2);
}

int ResolveLineAlignment(u8 text_position, u8 text_alignment)
{
	switch (text_alignment & 0x7)
	{
	case 1:
		return 0;
	case 3:
		return 2;
	case 0:
		return ResolveTextAnchorX(text_position);
	case 2:
	default:
		return 1;
	}
}

float ResolveBlockTop(float box_height, float text_height, int anchor_y)
{
	switch (anchor_y)
	{
	case 0:
		return 0.f;
	case 2:
		return box_height - text_height;
	case 1:
	default:
		return (box_height - text_height) * 0.5f;
	}
}

float ResolveLineStartX(float box_width, float line_width, int align_x)
{
	switch (align_x)
	{
	case 0:
		return 0.f;
	case 2:
		return box_width - line_width;
	case 1:
	default:
		return (box_width - line_width) * 0.5f;
	}
}

float ResolveBlockLeft(float box_width, float block_width, int anchor_x)
{
	return ResolveLineStartX(box_width, block_width, anchor_x);
}

} // namespace

void Textbox::Load(std::istream& file)
{
	Pane::Load(file);

	u16 text_buf_bytes, text_str_bytes;

	file >> BE >> text_buf_bytes >> text_str_bytes
		>> material_index >> font_index >> text_position >> text_alignment;

	file.seekg(2, std::ios::cur);

	u32 text_str_offset;

	file >> BE >> text_str_offset;

	ReadBEArray(file, &colors->r, sizeof(colors));

	file >> BE >> width >> height >> space_char >> space_line;

	// Read the BE UTF-16 string as u16 (the on-disk representation)
	// rather than as wchar_t directly: wchar_t is 32-bit on macOS/Linux
	// and only 16-bit on Windows, so the upstream `file >> BE >> wchar_t`
	// loop consumed 4 bytes per "character" on POSIX and merged
	// consecutive UTF-16 pairs into garbage codepoints. Reading into u16
	// and then widening to wchar_t works on every host.
	while (true)
	{
		u16 wch = 0;
		file >> BE >> wch;

		if (wch)
			text += static_cast<wchar_t>(wch);
		else
			break;
	}
}

void Textbox::Draw(const Resources& resources, u8 render_alpha, Vec2f adjust) const
{
	// The BRFNT path bypasses the material-driven shader pipeline
	// entirely, so don't apply the textbox's material when we can use
	// BRFNT glyphs.  If we fall through to the TextRenderer fallback
	// path, that path is similarly independent of the material, so the
	// material's tev stages are never the thing that makes text visible.
	const Font* font = nullptr;
	if (font_index < resources.fonts.size())
		font = resources.fonts[font_index];

	const bool brfnt_usable = font && font->IsUsable();

	const u8 r  = colors[0].r;
	const u8 g  = colors[0].g;
	const u8 b  = colors[0].b;
	const u8 a  = MultiplyColors(colors[0].a, render_alpha);
	const u8 r2 = colors[1].r;
	const u8 g2 = colors[1].g;
	const u8 b2 = colors[1].b;
	const u8 a2 = MultiplyColors(colors[1].a, render_alpha);

	glPushMatrix();

	// Pane::Render left the modelview at the textbox's origin. Move
	// local (0, 0) to the top-left of the textbox so the rest of the
	// draw code can lay out glyphs in simple (+X right, +Y down) box
	// coordinates. This is the same correction Quad::Draw applies for
	// picture/window panes.
	glTranslatef(
		-GetWidth()  / 2.f * static_cast<float>(GetOriginX()),
		-GetHeight() / 2.f * static_cast<float>(GetOriginY()),
		0.f);

	if (brfnt_usable)
	{
		const int text_anchor_x = ResolveTextAnchorX(text_position);
		const int text_anchor_y = ResolveTextAnchorY(text_position);
		const int line_align_x = ResolveLineAlignment(text_position, text_alignment);

		// Scale from the font's nominal FINF metrics when available.
		// BRFNT cell dimensions include atlas padding around the glyph,
		// so sizing directly from cell_width/cell_height makes the text
		// look vertically off compared with the Wii's own layout.
		const float native_font_w = font->font_width > 0
			? static_cast<float>(font->font_width)
			: static_cast<float>(font->cell_width);
		const float native_font_h = font->font_height > 0
			? static_cast<float>(font->font_height)
			: static_cast<float>(font->cell_height);
		const float scale_x = (width > 0.f && native_font_w > 0.f)
			? width / native_font_w : 1.f;
		const float scale_y = (height > 0.f && native_font_h > 0.f)
			? height / native_font_h : 1.f;
		const float max_native_width = scale_x > 0.f
			? GetWidth() / scale_x
			: GetWidth();
		const std::vector<WrappedLine> lines = WrapLines(
			text,
			max_native_width,
			space_char,
			[font](wchar_t wc)
			{
				const u16 gi = font->GetGlyphIndex(static_cast<u16>(wc));
				return static_cast<float>(font->GetCharWidth(gi).char_width);
			});
		float max_line_width = 0.f;
		for (const WrappedLine& line : lines)
			max_line_width = std::max(max_line_width, line.width * scale_x);

		const float ascent_from_top =
			(font->font_ascent > 0
				? static_cast<float>(font->font_ascent)
				: (font->baseline_pos > 0
					? static_cast<float>(font->baseline_pos)
					: native_font_h))
			* scale_y;
		const float glyph_baseline =
			(font->baseline_pos > 0
				? static_cast<float>(font->baseline_pos)
				: (font->font_ascent > 0
					? static_cast<float>(font->font_ascent)
					: native_font_h))
			* scale_y;
		const float line_height_native =
			(font->linefeed > 0 ? static_cast<float>(font->linefeed)
			                   : (font->font_height > 0
					   ? static_cast<float>(font->font_height)
					   : static_cast<float>(font->cell_height)));
		const float line_box_h = native_font_h * scale_y;
		const float line_step = line_height_native * scale_y + space_line;
		const float text_height = line_box_h
			+ (lines.size() > 1 ? (lines.size() - 1) * line_step : 0.f);
		const float block_left = ResolveBlockLeft(GetWidth(), max_line_width, text_anchor_x);
		const float block_top = ResolveBlockTop(GetHeight(), text_height, text_anchor_y);
		const float first_baseline_y = block_top + ascent_from_top;

		// The textbox's local frame inherits +Y-up from its parent
		// panes.  Flip Y around the centre so our +Y-down glyph layout
		// renders right-side-up in the banner frame. The TextRenderer
		// fallback applies an identical flip.
		glTranslatef(0.f, GetHeight() * 0.5f, 0.f);
		glScalef(1.f, -1.f, 1.f);
		glTranslatef(0.f, -GetHeight() * 0.5f, 0.f);

		// Stop the tev-compiled shader program (from the last material
		// draw) from intercepting our texture samples. glUseProgram(0)
		// reverts to the legacy 2.1 fixed-function pipeline.
		GLint saved_program = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
		glUseProgram(0);

		// Some previous material draws may have enabled textures on
		// additional units; turn them off before we set up the simple
		// fixed-function state on unit 0.
		for (GLenum unit = GL_TEXTURE1; unit <= GL_TEXTURE7; ++unit)
		{
			glActiveTexture(unit);
			glBindTexture(GL_TEXTURE_2D, 0);
			glDisable(GL_TEXTURE_2D);
		}
		glActiveTexture(GL_TEXTURE0);
		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
		glMatrixMode(GL_MODELVIEW);
		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

		// Standard premultiplied-style alpha blending against the
		// already-rendered banner background.
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		// 0.5-texel inset on UVs to keep bilinear sampling from
		// bleeding the next glyph cell into the current one when the
		// cells are packed tight against each other in the sheet.
		const float eps_u = 0.5f / static_cast<float>(font->sheet_width);
		const float eps_v = 0.5f / static_cast<float>(font->sheet_height);

		int bound_sheet = -1;
		const u16 sheet_count = font->GetSheetCount();
		const u32 max_glyph =
			static_cast<u32>(font->glyphs_per_sheet) * sheet_count;

		for (std::size_t line_index = 0; line_index < lines.size(); ++line_index)
		{
			const float line_width = lines[line_index].width * scale_x;
			float pen_x = block_left + ResolveLineStartX(
				max_line_width, line_width, line_align_x);

			const float baseline_y =
				first_baseline_y + static_cast<float>(line_index) * line_step;

			for (wchar_t wc : lines[line_index].text)
			{
				const u16 gi = font->GetGlyphIndex(static_cast<u16>(wc));
				const BrfntCharWidth cw = font->GetCharWidth(gi);
				if (gi < max_glyph)
				{
					const u16 sheet_idx = static_cast<u16>(gi / font->glyphs_per_sheet);
					const u16 local_idx = static_cast<u16>(gi % font->glyphs_per_sheet);
					const u16 col = static_cast<u16>(local_idx % font->sheet_row);
					const u16 row = static_cast<u16>(local_idx / font->sheet_row);

					if (static_cast<int>(sheet_idx) != bound_sheet)
					{
						font->BindSheet(sheet_idx);
						bound_sheet = sheet_idx;
					}

					const float cell_stride_w = font->sheet_row > 0
						? static_cast<float>(font->sheet_width) / font->sheet_row
						: static_cast<float>(font->cell_width);
					const float cell_stride_h = font->sheet_line > 0
						? static_cast<float>(font->sheet_height) / font->sheet_line
						: static_cast<float>(font->cell_height);
					const float cell_origin_x = col * cell_stride_w + 1.f;
					const float cell_origin_y = row * cell_stride_h + 1.f;
					const float u0 = cell_origin_x
						/ static_cast<float>(font->sheet_width) + eps_u;
					const float v0 = cell_origin_y
						/ static_cast<float>(font->sheet_height) + eps_v;
					const float glyph_w = cw.glyph_width > 0
						? static_cast<float>(cw.glyph_width)
						: static_cast<float>(font->cell_width);
					const float u1 = (cell_origin_x + glyph_w)
						/ static_cast<float>(font->sheet_width) - eps_u;
					const float v1 = (cell_origin_y + static_cast<float>(font->cell_height))
						/ static_cast<float>(font->sheet_height) - eps_v;

					const float glyph_h = static_cast<float>(font->cell_height) * scale_y;

					const float gx0 = pen_x + static_cast<float>(cw.left) * scale_x;
					const float gx1 = gx0 + glyph_w * scale_x;
					const float gy0 = baseline_y - glyph_baseline;
					const float gy1 = gy0 + glyph_h;

					glBegin(GL_QUADS);
					glColor4ub(r,  g,  b,  a);
					glTexCoord2f(u0, v0); glVertex2f(gx0, gy0);
					glColor4ub(r,  g,  b,  a);
					glTexCoord2f(u1, v0); glVertex2f(gx1, gy0);
					glColor4ub(r2, g2, b2, a2);
					glTexCoord2f(u1, v1); glVertex2f(gx1, gy1);
					glColor4ub(r2, g2, b2, a2);
					glTexCoord2f(u0, v1); glVertex2f(gx0, gy1);
					glEnd();
				}

				pen_x += (static_cast<float>(cw.char_width) + space_char) * scale_x;
			}
		}

		// Unbind our font sheet and re-enable whatever tev shader the
		// next draw expects. We leave GL_TEXTURE0 bound to nothing so
		// the next material's texture setup starts from a clean slate.
		glBindTexture(GL_TEXTURE_2D, 0);
		if (saved_program)
			glUseProgram(static_cast<GLuint>(saved_program));
	}
	else if (TextRenderer* tr = TextRenderer::GetOrInit())
	{
		// No real Wii font available; fall back to the bundled TTF.
		// DrawString places its glyphs starting from the current modelview
		// origin, which we have already translated to the textbox top-left.
		tr->DrawString(
			text,
			GetWidth(),
			GetHeight(),
			text_position,
			text_alignment,
			space_line,
			r, g, b, a);
	}

	glPopMatrix();
}

}
