export module font_loader;

import <cstdint>;
import std;

export constexpr std::string_view FONTS_PATH = "resources\\fonts\\";

export struct character_info
{
	uint32_t width;
	uint32_t height;
	uint32_t bearing_width;
	uint32_t bearing_height;
	uint32_t advance;
	std::vector<uint8_t> buffer;
};

export class font_loader
{
public:
	const std::vector<character_info> load_font(const std::filesystem::path& _font_path, uint32_t _font_size, const std::wstring& _characters);
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