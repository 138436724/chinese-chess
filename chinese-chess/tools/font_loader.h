#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#define FONT_LOADER font_loader::get_font_loader()
constexpr std::u8string_view FONTS_PATH = u8"resources\\fonts\\";

struct character_info
{
	uint32_t width;
	uint32_t height;
	uint32_t bearing_width;
	uint32_t bearing_height;
	uint32_t advance;
	std::vector<uint8_t> buffer;
};

class font_loader
{
public:
	std::vector<character_info> load_font(const std::filesystem::path& _font_path, uint32_t _font_size, const std::wstring& _characters);
	static font_loader& get_font_loader() noexcept;

private:
	font_loader() = default;
	~font_loader() = default;

	font_loader(const font_loader&) = delete;
	font_loader& operator=(const font_loader&) = delete;
	font_loader(const font_loader&&) = delete;
	font_loader& operator=(const font_loader&&) = delete;

	static font_loader loader;
};