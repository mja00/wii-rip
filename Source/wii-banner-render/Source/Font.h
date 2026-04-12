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

// NOTE: Altered source version of Font.h from the wii-banner-player project.
// Modifications:
//   - Replaced the single-texture upstream representation with a proper
//     multi-sheet BRFNT container: Font now owns one GL texture per sheet,
//     a character-code -> glyph-index map populated from every CMAP
//     sub-section, and a per-glyph CharWidth table populated from every
//     CWDH sub-section.  FINF and TGLP metadata are exposed as public
//     fields so Textbox::Draw can compute per-character UVs, per-character
//     advances and per-textbox scaling from the loaded font.
//   - Apply() is now a no-op: the font-rendering path in Textbox::Draw
//     bypasses the material / tev / shader pipeline (WrapGx.cpp) and draws
//     glyph quads directly against the bound font sheet texture.  See the
//     NOTE block at the top of Textbox.cpp for why.

#ifndef WII_BNR_FONT_H_
#define WII_BNR_FONT_H_

#include <map>
#include <vector>

#include "Pane.h"

namespace WiiBanner
{

// Per-glyph horizontal metrics pulled from a CWDH section.
//   left        signed offset from pen to left edge of glyph
//   glyph_width unsigned width of the glyph image itself
//   char_width  total advance ( left + glyph_width + right_bearing )
struct BrfntCharWidth
{
	s8 left;
	u8 glyph_width;
	s8 char_width;
};

class Font : public Named
{
public:
	Font() = default;
	~Font();

	void Load(std::istream& file);

	// Legacy entry point. The new BRFNT path in Textbox::Draw manages its
	// own GL state and binds sheets on demand via BindSheet(), so there's
	// nothing for Apply() to do. Kept as a no-op so upstream callers still
	// compile.
	void Apply() const {}

	// Has enough metadata been parsed from a real BRFNT to draw glyphs?
	// Textbox::Draw falls back to the bundled TTF renderer when this is
	// false.
	bool IsUsable() const
	{
		return !sheet_textures.empty()
			&& cell_width > 0 && cell_height > 0
			&& sheet_row > 0 && sheet_line > 0
			&& sheet_width > 0 && sheet_height > 0
			&& glyphs_per_sheet > 0;
	}

	u16 GetSheetCount() const { return static_cast<u16>(sheet_textures.size()); }

	// Maps a codepoint to a glyph index, using the parsed CMAP chain.
	// Returns alter_char_index when the codepoint isn't covered by any
	// CMAP (the BRFNT designer's "unknown glyph" fallback).
	u16 GetGlyphIndex(u16 char_code) const;

	// Returns the per-glyph advance/width metrics, or default_char_width
	// when the glyph index falls outside the parsed CWDH range.
	BrfntCharWidth GetCharWidth(u16 glyph_index) const;

	// Binds the requested sheet as GL_TEXTURE_2D on the currently active
	// texture unit. The caller is expected to have prepared raw
	// fixed-function GL state (see the BRFNT path in Textbox::Draw).
	void BindSheet(u16 sheet_index) const;

	// Glyph sheet metadata (TGLP).
	u8  cell_width       = 0;
	u8  cell_height      = 0;
	s8  baseline_pos     = 0;
	u8  max_char_width   = 0;
	u16 sheet_row        = 0;   // glyph cells per row within one sheet
	u16 sheet_line       = 0;   // glyph cell rows within one sheet
	u16 sheet_width      = 0;   // sheet texture width in pixels
	u16 sheet_height     = 0;   // sheet texture height in pixels
	u16 sheet_format     = 0;
	u16 glyphs_per_sheet = 0;

	// Font information (FINF).
	s8  linefeed          = 0;
	u16 alter_char_index  = 0;
	BrfntCharWidth default_char_width = { 0, 0, 0 };
	u8  font_height       = 0;
	u8  font_width        = 0;
	u8  font_ascent       = 0;

	// Legacy convenience preserved for any remaining upstream callers:
	// the lowest codepoint covered by the first CMAP encountered. The
	// main rendering path uses char_map / GetGlyphIndex() and ignores
	// this.
	u16 char_offset = 0;

private:
	// One GL texture per sheet. Stored as unsigned int so this header
	// doesn't need to pull in <GL/glew.h>.
	std::vector<unsigned int> sheet_textures;

	// Character code -> glyph index. Populated from every CMAP section
	// (methods 0, 1 and 2 are all handled).
	std::map<u16, u16> char_map;

	// Per-glyph width table. glyph_widths[i] covers glyph index
	// (cwdh_start_index + i). Populated from every CWDH section in the
	// file and merged into a single dense vector.
	u16 cwdh_start_index = 0;
	std::vector<BrfntCharWidth> glyph_widths;
};

class FontList : public std::vector<Font*>
{
public:
	static const u32 BINARY_MAGIC = MAKE_FOURCC('f', 'n', 'l', '1');
};

}

#endif
