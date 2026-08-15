#include "font_loader.h"

#include "string_helper.h"

#include <format>
#include <freetype/freetype.h>
#include <memory>
#include <type_traits>

std::expected<std::vector<character_info>, std::string> font_loader::load_font(const std::filesystem::path& _font_path,
                                                                               uint32_t                     _font_size,
                                                                               std::wstring_view            _characters)
{
    FT_Library ft = nullptr;
    if (const auto error = FT_Init_FreeType(&ft); error)
    {
        return std::unexpected(std::format("Could not init FreeType Library. Error code {}.", error));
    }
    const std::unique_ptr<std::remove_pointer_t<FT_Library>, decltype(&FT_Done_FreeType)> ft_guard(ft, FT_Done_FreeType);

    FT_Face face = nullptr;
    if (const auto error = FT_New_Face(ft, _font_path.generic_string().c_str(), 0, &face); error)
    {
        return std::unexpected(std::format("Failed to load font {}. Error code {}.", _font_path.generic_string(), error));
    }
    const std::unique_ptr<std::remove_pointer_t<FT_Face>, decltype(&FT_Done_Face)> face_guard(face, FT_Done_Face);

    FT_Set_Pixel_Sizes(face, 0, _font_size);

    std::vector<character_info> character_infos;
    character_infos.reserve(_characters.size());

    for (const auto& _char : _characters)
    {
        if (const auto error = FT_Load_Char(face, _char, FT_LOAD_RENDER); error)
        {
            const auto error_string = std::format(L"Failed to load glyph '{}'. Error code {}.", _char, error);
            return std::unexpected(string_helper::convert_to<std::string, std::wstring>(error_string));
        }

        const uint32_t width  = face->glyph->bitmap.width;
        const uint32_t height = face->glyph->bitmap.rows;
        const uint8_t* bitmap = face->glyph->bitmap.buffer;

        character_infos.emplace_back(character_info(width, height, face->glyph->bitmap_left, face->glyph->bitmap_top,
                                                    (face->glyph->advance.x + 32) / 64,
                                                    std::vector<uint8_t>(bitmap, bitmap + width * height)));
    }

    return character_infos;
}
