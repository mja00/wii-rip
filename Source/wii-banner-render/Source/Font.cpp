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

// NOTE: Altered source version of Font.cpp from the wii-banner-player project.
// Modifications:
//   - Accepts both 'RFNA' (Revolution shared-font magic found inside the
//     Wii NAND at 00000011.app) and 'RFNT' (channel banner font magic) as
//     valid BRFNT headers.  Upstream only accepted 'RFNA' and rejected
//     every channel-embedded font with a "bad header" early return.
//   - Fixed an off-by-one read in the TGLP (Texture Glyph) section: the
//     original `file >> ... >> sheet_row >> sheet_row >> sheet_line >> ...`
//     read `sheet_row` twice, which shifted every subsequent field by two
//     bytes and corrupted sheet_line / sheet_width / sheet_height /
//     sheet_image.  The duplicate read is gone so all TGLP fields land in
//     their correct slots.
//   - sheet_image is now read as a big-endian u32 (matching the rest of
//     the BRFNT / NW4R layout) and is seeked relative to file_start.  The
//     upstream code read it as LE and then seeked absolutely from the
//     enclosing stream's origin, which landed on the wrong byte every
//     time and silently corrupted every loaded texture sheet.
//   - Instead of loading a single 'first sheet' and dropping everything
//     else, Load() now decodes EVERY sheet via TexDecoder_Decode (with
//     rgbaOnly=true so the I4/I8/IA4/IA8 paths all emit RGBA where
//     intensity is replicated into the alpha channel) and uploads each
//     sheet as its own plain GL_TEXTURE_2D object.  Textbox::Draw picks
//     the correct sheet per glyph at draw time.
//   - Parses the full CMAP linked list with support for all three NW4R
//     mapping methods (0 = direct, 1 = table, 2 = scan), populating a
//     char_code -> glyph_index std::map.  Upstream discarded everything
//     but the starting codepoint of the first CMAP entry, which is why
//     text only rendered for ASCII-range linear fonts and broke as soon
//     as the channel author used a sparse CMAP.
//   - Parses the full CWDH linked list and merges every sub-section into
//     a dense per-glyph CharWidth table.  Textbox::Draw uses these for
//     per-character advances and left-bearing offsets, instead of
//     hard-advancing by cell_width.
//   - Removed the testing line `sheet_height = sheet_line * cell_height;`
//     which clobbered the value just read from the file.
//   - Font::Apply() is now a no-op.  The BRFNT rendering path in
//     Textbox::Draw manages its own GL state and binds sheets on demand.

#include <GL/glew.h>

#include <algorithm>
#include <vector>

#include "Font.h"
#include "Endian.h"

// from dolphin
#include "TextureDecoder.h"

namespace WiiBanner
{

enum BinaryMagic : u32
{
	// 'RFNA' is the original Revolution-era system font magic (used by
	// Wii system shared1 fonts like wbf1.brfna). 'RFNT' is the more
	// common BRFNT magic shipped inside individual channel banner
	// archives. Both formats share the same NW4R font layout and version
	// 0x0104 header, so we accept either magic for the same loader.
	BINARY_MAGIC_FONT_RFNA = MAKE_FOURCC('R', 'F', 'N', 'A'),
	BINARY_MAGIC_FONT_RFNT = MAKE_FOURCC('R', 'F', 'N', 'T'),

	BINARY_MAGIC_GLYPH_GROUP        = MAKE_FOURCC('G', 'L', 'G', 'R'),
	BINARY_MAGIC_FONT_INFORMATION   = MAKE_FOURCC('F', 'I', 'N', 'F'),
	BINARY_MAGIC_TEXTURE_GLYPH      = MAKE_FOURCC('T', 'G', 'L', 'P'),
	BINARY_MAGIC_CHARACTER_CODE_MAP = MAKE_FOURCC('C', 'M', 'A', 'P'),
	BINARY_MAGIC_CHARACTER_WIDTH    = MAKE_FOURCC('C', 'W', 'D', 'H')
};

Font::~Font()
{
	if (!sheet_textures.empty())
	{
		// Safe to call after the GL context is gone: glDeleteTextures on a
		// nonexistent name is a no-op. In practice Font objects are owned
		// by a Layout which is deleted while the context is still alive.
		glDeleteTextures(static_cast<GLsizei>(sheet_textures.size()),
			sheet_textures.data());
		sheet_textures.clear();
	}
}

u16 Font::GetGlyphIndex(u16 char_code) const
{
	const auto it = char_map.find(char_code);
	if (it != char_map.end())
		return it->second;
	return alter_char_index;
}

BrfntCharWidth Font::GetCharWidth(u16 glyph_index) const
{
	if (glyph_index >= cwdh_start_index)
	{
		const std::size_t offset = static_cast<std::size_t>(
			glyph_index - cwdh_start_index);
		if (offset < glyph_widths.size())
			return glyph_widths[offset];
	}
	return default_char_width;
}

void Font::BindSheet(u16 sheet_index) const
{
	if (sheet_index < sheet_textures.size())
		glBindTexture(GL_TEXTURE_2D, sheet_textures[sheet_index]);
}

void Font::Load(std::istream& file)
{
	const std::streamoff file_start = file.tellg();

	// Header.
	FourCC header_magic;
	u16 endian;
	u16 version;
	u32 filesize;
	u16 offset;        // offset to first section
	u16 section_count;

	file >> header_magic >> BE >> endian >> version
		>> filesize >> offset >> section_count;

	if ((header_magic != BINARY_MAGIC_FONT_RFNA
			&& header_magic != BINARY_MAGIC_FONT_RFNT)
		|| endian != 0xFEFF
		|| version != 0x0104)
		return;	// bad header

	// Seek to the first section.
	file.seekg(file_start + offset, std::ios::beg);

	ReadSections(file, section_count,
		[&](FourCC magic, std::streamoff /*section_start*/)
	{
		if (magic == BINARY_MAGIC_FONT_INFORMATION)
		{
			u8  font_type;
			s8  default_left;
			u8  default_glyph;
			s8  default_char;
			u8  encoding;
			u32 pGlyph, pWidth, pMap;

			file >> BE >> font_type >> linefeed >> alter_char_index
				>> default_left >> default_glyph >> default_char
				>> encoding >> pGlyph >> pWidth >> pMap
				>> font_height >> font_width >> font_ascent;

			default_char_width.left        = default_left;
			default_char_width.glyph_width = default_glyph;
			default_char_width.char_width  = default_char;
		}
		else if (magic == BINARY_MAGIC_TEXTURE_GLYPH)
		{
			u32 sheet_size;
			u16 sheet_count_local;
			u32 sheet_image;

			file >> BE >> cell_width >> cell_height
				>> baseline_pos >> max_char_width
				>> sheet_size >> sheet_count_local >> sheet_format
				>> sheet_row >> sheet_line >> sheet_width >> sheet_height
				>> sheet_image;

			if (!sheet_count_local || !sheet_size
				|| !sheet_row || !sheet_line
				|| !sheet_width || !sheet_height)
				return;

			glyphs_per_sheet = static_cast<u16>(sheet_row * sheet_line);

			// Pull the raw (GX-encoded) bytes for every sheet at once.
			// sheet_image is relative to the start of the BRFNT file.
			file.seekg(file_start + sheet_image, std::ios::beg);

			std::vector<u8> all_encoded(
				static_cast<std::size_t>(sheet_size) * sheet_count_local);
			file.read(reinterpret_cast<char*>(all_encoded.data()),
				static_cast<std::streamsize>(all_encoded.size()));

			// Decode each sheet into RGBA8 and upload it as its own GL
			// texture. TexDecoder_Decode with rgbaOnly=true emits
			// RGBA for every supported format; for the I4/I8/IA4/IA8
			// formats the NW4R shared/channel fonts use, intensity is
			// replicated across RGBA so the texture doubles as an alpha
			// mask when modulated by glColor.
			const u8 gx_format = static_cast<u8>(sheet_format & 0x7fff);

			sheet_textures.assign(sheet_count_local, 0u);
			glGenTextures(
				static_cast<GLsizei>(sheet_count_local),
				sheet_textures.data());

			std::vector<u32> decoded(
				static_cast<std::size_t>(sheet_width) * sheet_height);

			// Make sure row alignment matches our tightly-packed RGBA
			// buffer. (Some GL drivers default to 4-byte row alignment
			// which is fine for RGBA8 but sets a bad precedent.)
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

			for (u16 i = 0; i < sheet_count_local; ++i)
			{
				TexDecoder_Decode(
					reinterpret_cast<u8*>(decoded.data()),
					all_encoded.data() + static_cast<std::size_t>(i) * sheet_size,
					sheet_width, sheet_height,
					gx_format, 0, 0, /*rgbaOnly=*/true);

				glBindTexture(GL_TEXTURE_2D, sheet_textures[i]);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
					sheet_width, sheet_height, 0,
					GL_RGBA, GL_UNSIGNED_BYTE, decoded.data());
			}
		}
		else if (magic == BINARY_MAGIC_CHARACTER_WIDTH)
		{
			u16 index_begin;
			u16 index_end;
			u32 pNext;

			file >> BE >> index_begin >> index_end >> pNext;

			if (index_end < index_begin)
				return;

			const u32 count = static_cast<u32>(index_end) - index_begin + 1;
			std::vector<BrfntCharWidth> widths(count);
			for (u32 i = 0; i < count; ++i)
			{
				file >> BE >> widths[i].left
					>> widths[i].glyph_width
					>> widths[i].char_width;
			}

			// Merge into the running dense vector. CWDH sections are
			// usually contiguous but we tolerate gaps by seeding the
			// gap with default_char_width.
			if (glyph_widths.empty())
			{
				cwdh_start_index = index_begin;
				glyph_widths = std::move(widths);
			}
			else
			{
				const u16 old_start = cwdh_start_index;
				const u16 old_end   = static_cast<u16>(
					old_start + glyph_widths.size() - 1);
				const u16 new_start = std::min(old_start, index_begin);
				const u16 new_end   = std::max(old_end, index_end);
				const std::size_t new_size =
					static_cast<std::size_t>(new_end - new_start) + 1;

				std::vector<BrfntCharWidth> merged(
					new_size, default_char_width);
				for (std::size_t i = 0; i < glyph_widths.size(); ++i)
					merged[(old_start - new_start) + i] = glyph_widths[i];
				for (std::size_t i = 0; i < widths.size(); ++i)
					merged[(index_begin - new_start) + i] = widths[i];

				cwdh_start_index = new_start;
				glyph_widths = std::move(merged);
			}
		}
		else if (magic == BINARY_MAGIC_CHARACTER_CODE_MAP)
		{
			u16 ccode_begin;
			u16 ccode_end;
			u16 mapping_method;
			u32 pNext;

			file >> BE >> ccode_begin >> ccode_end >> mapping_method;
			file.seekg(2, std::ios::cur);  // 2-byte padding before pNext
			file >> BE >> pNext;

			// Legacy convenience: remember the first CMAP's starting
			// codepoint. Used to be the only thing upstream stored.
			if (char_map.empty())
				char_offset = ccode_begin;

			if (ccode_end < ccode_begin)
				return;

			switch (mapping_method)
			{
			case 0:  // DIRECT - one u16 `first_glyph_index`
			{
				u16 first_glyph;
				file >> BE >> first_glyph;
				for (u32 c = ccode_begin; c <= ccode_end; ++c)
				{
					char_map[static_cast<u16>(c)] =
						static_cast<u16>(first_glyph + (c - ccode_begin));
				}
				break;
			}
			case 1:  // TABLE - (end - begin + 1) u16 glyph indices
			{
				const u32 count = static_cast<u32>(ccode_end) - ccode_begin + 1;
				for (u32 i = 0; i < count; ++i)
				{
					u16 glyph_idx;
					file >> BE >> glyph_idx;
					if (glyph_idx != 0xFFFF)
					{
						char_map[static_cast<u16>(ccode_begin + i)] = glyph_idx;
					}
				}
				break;
			}
			case 2:  // SCAN - u16 info_count, then (ccode, glyph) pairs
			{
				u16 info_count;
				file >> BE >> info_count;
				for (u16 i = 0; i < info_count; ++i)
				{
					u16 code;
					u16 glyph_idx;
					file >> BE >> code >> glyph_idx;
					char_map[code] = glyph_idx;
				}
				break;
			}
			default:
				// Unknown mapping method: skip and keep going.
				break;
			}
		}
		// Silently ignore GLGR and any other unrecognised top-level
		// sections; ReadSections will seek past them.
	});
}

}
