#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

constexpr std::u8string_view FONTS_PATH = u8"resources\\fonts\\";

struct character_info
{
    uint32_t             width          = 0;
    uint32_t             height         = 0;
    uint32_t             bearing_width  = 0;
    uint32_t             bearing_height = 0;
    uint32_t             advance        = 0;
    std::vector<uint8_t> buffer;
};

namespace font_loader {

[[nodiscard]] std::vector<character_info> load_font(const std::filesystem::path& _font_path,
                                                    uint32_t                     _font_size,
                                                    const std::wstring&          _characters);

}  // namespace font_loader
