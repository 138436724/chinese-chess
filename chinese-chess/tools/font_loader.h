/**
 * @file font_loader.h
 * @brief FreeType 字形光栅化：把一段宽字符按像素字号渲染成位图与排版度量。
 * @details 只产出 8 位灰度覆盖度，颜色与图集拼装由调用方（scene_texture_manager 的字形图集路径）负责。
 *          返回数组与传入字符严格等长同序，重复字符各自渲染一份，不去重。FONTS_PATH 是运行时的
 *          字体目录常量，相对运行目录 chinese-chess/。
 */

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

constexpr std::string_view FONTS_PATH = "resources\\fonts\\";

/**
 * @brief 一个已光栅化的字形：位图尺寸、基线位移、步进与 8 位灰度像素。
 * @details width / height 是位图本身的像素数，可能为 0（例如空格）；bearing_width / bearing_height
 *          是相对基线的左上角偏移（FreeType 的 bitmap_left / bitmap_top）；advance 已从 26.6 定点数
 *          四舍五入成整数像素；buffer 按行紧密排列，长度为 width * height。
 */
struct character_info
{
    /** @brief 位图宽度（像素），即 FreeType 的 bitmap.width。 */
    uint32_t             width          = 0;
    /** @brief 位图高度（像素），即 FreeType 的 bitmap.rows。 */
    uint32_t             height         = 0;
    /** @brief 相对基线的水平偏移，直接取 FreeType 的 bitmap_left。 */
    uint32_t             bearing_width  = 0;
    /** @brief 相对基线的垂直偏移，直接取 FreeType 的 bitmap_top。 */
    uint32_t             bearing_height = 0;
    /** @brief 水平步进（像素），由 advance.x 的 26.6 定点数加 32 后整除 64 得到。 */
    uint32_t             advance        = 0;
    /** @brief 8 位灰度覆盖度，行紧密排列，长度恰为 width * height。 */
    std::vector<uint8_t> buffer;
};

namespace font_loader {

/**
 * @brief 用 FreeType 把一串字符按指定像素字号渲染成字形位图。
 * @param _font_path 字体文件路径（TTF / OTF），内部转成 generic_string 交给 FT_New_Face。
 * @param _font_size 字号，作为 FT_Set_Pixel_Sizes 的像素高度（宽度传 0，表示按高度定尺寸）。
 * @param _characters 待渲染的宽字符序列，返回数组与它等长同序。
 * @return 每个字符的 character_info；初始化库、打开字体或加载任一字形失败都返回错误串。
 * @note 每个字形都用 FT_LOAD_RENDER 渲染成 8 位灰度位图，位图按行拷进 buffer。
 * @note 每次调用都新建 FT_Library 与 FT_Face，代价随字体文件大小，不在字符循环里。
 */
[[nodiscard]] std::expected<std::vector<character_info>, std::string> load_font(const std::filesystem::path& _font_path,
                                                                                uint32_t                     _font_size,
                                                                                std::wstring_view _characters);

}  // namespace font_loader
