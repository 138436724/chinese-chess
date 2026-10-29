/**
 * @file font_loader.h
 * @brief FreeType 字形光栅化：把一段宽字符按像素字号渲染成位图与排版度量。
 * @details 按 8 位灰度覆盖度解释字形，颜色与图集拼装由调用方（scene_texture_manager 的字形图集路径）负责。
 *          成功结果与输入 wchar_t 码元等长同序，重复码元不去重；Windows 上不合并 UTF-16 代理对。FONTS_PATH 是运行时的
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
 *          保存 bitmap_left / bitmap_top 转成 uint32_t 的值，负偏移无法正确表示；advance 用
 *          (advance.x + 32) / 64 转成像素。buffer 连续复制 width * height 字节，未处理 pitch 或像素模式。
 */
struct character_info
{
    uint32_t             width          = 0;  ///< @brief 位图宽度（像素），即 FreeType 的 bitmap.width
    uint32_t             height         = 0;  ///< @brief 位图高度（像素），即 FreeType 的 bitmap.rows
    uint32_t             bearing_width  = 0;  ///< @brief bitmap_left 转成 uint32_t 的水平偏移；原值为负时会回绕
    uint32_t             bearing_height = 0;  ///< @brief bitmap_top 转成 uint32_t 的垂直偏移，向上为正；原值为负时会回绕
    uint32_t             advance        = 0;  ///< @brief 水平步进（像素），由 advance.x 的 26.6 定点数加 32 后整除 64 得到
    std::vector<uint8_t> buffer;  ///< @brief 连续复制的 width * height 字节，按紧密灰度像素使用
};

namespace font_loader {

/**
 * @brief 用 FreeType 把一串字符按指定像素字号渲染成字形位图。
 * @param _font_path 字体文件路径（TTF / OTF），内部转成 generic_string 交给 FT_New_Face。
 * @param _font_size 字号，作为 FT_Set_Pixel_Sizes 的像素高度（宽度传 0，表示按高度定尺寸）。
 * @param _characters 待渲染的宽字符序列，返回数组与它等长同序。
 * @return 每个字符的 character_info；初始化库、打开字体或加载任一字形失败都返回错误串。
 * @note 用 FT_LOAD_RENDER 加载字形，连续复制 width * height 字节；正确解释要求灰度格式且 pitch == width。
 * @note 每次调用在字符循环外新建 FT_Library 与 FT_Face；未检查 FT_Set_Pixel_Sizes 的返回值。
 * @note expected 只承载显式检查的 FreeType 错误，内存分配等异常仍可能传播。
 */
[[nodiscard]] std::expected<std::vector<character_info>, std::string> load_font(const std::filesystem::path& _font_path,
                                                                                uint32_t                     _font_size,
                                                                                std::wstring_view _characters);

}  // namespace font_loader
