/**
 * @file font_loader.cpp
 * @brief font_loader 的实现：每次调用独立初始化 FreeType 库与 face，并用 unique_ptr 守卫释放。
 * @details 错误信息统一用 std::format 拼成英文；字形加载失败那条带宽字符，先经 string_helper::convert_to
 *          转成 UTF-8 再放进 expected 的 error。位图连续复制 width * height 字节，未按 pitch 逐行转换。
 */

#include "font_loader.h"

#include "string_helper.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <freetype/freetype.h>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

/**
 * @brief 初始化 FreeType、打开字体，并逐个字符渲染成灰度位图与排版度量。
 * @note 库与 face 都用 unique_ptr 加自定义删除器守护，任何提前返回都不会泄漏。
 * @note FT_Set_Pixel_Sizes(face, 0, _font_size) 将像素宽度设为 0、高度设为 _font_size；未检查返回码。
 * @note 字形错误码原样拼进错误串，调用方看到的失败原因来自 FreeType 而不是推断。
 */
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

        // 当前拷贝假定正向、紧密的灰度位图；未处理 pitch 填充、负 pitch 或其他像素模式。
        // 当前拷贝假定正向、紧密的灰度位图；未处理 pitch 填充、负 pitch 或其他像素模式。
        character_infos.emplace_back(width, height, face->glyph->bitmap_left, face->glyph->bitmap_top,
                                     (face->glyph->advance.x + 32) / 64, std::vector<uint8_t>(bitmap, bitmap + width * height));
    }

    return character_infos;
}
