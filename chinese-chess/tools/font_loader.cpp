#include "font_loader.h"
#include <freetype/freetype.h>
#include <ranges>

font_loader font_loader::loader;

std::vector<character_info> font_loader::load_font(const std::filesystem::path& _font_path, uint32_t _font_size, const std::wstring& _characters)
{
	FT_Library ft;
	if (FT_Init_FreeType(&ft))
	{
		throw std::runtime_error("Could not init FreeType Library.");
	}

	FT_Face face;
	if (FT_New_Face(ft, _font_path.generic_string().c_str(), 0, &face))
	{
		throw std::runtime_error("Failed to load font.");
	}

	FT_Set_Pixel_Sizes(face, 0, _font_size);

	std::vector<character_info> character_infos;
	character_infos.reserve(_characters.size());

	for (const auto& _char : _characters)
	{
		if (FT_Load_Char(face, _char, FT_LOAD_RENDER))
		{
			throw std::runtime_error("Failed to load Glyph.");
		}

		const uint32_t width = face->glyph->bitmap.width;
		const uint32_t height = face->glyph->bitmap.rows;
		const uint8_t* bitmap = face->glyph->bitmap.buffer;

		character_infos.emplace_back(
			character_info(width, height, face->glyph->bitmap_left, face->glyph->bitmap_top, face->glyph->advance.x / 64, std::vector<uint8_t>(bitmap, bitmap + width * height))
		);
	}

	FT_Done_Face(face);
	FT_Done_FreeType(ft);

	return character_infos;
}

font_loader& font_loader::get_font_loader() noexcept
{
	return loader;
}
