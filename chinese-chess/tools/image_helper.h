#pragma once

#include <cstdint>
#include <filesystem>
#include <OpenImageIO/typedesc.h>
#include <string_view>

#define IMAGE_HELPER image_helper::get_image_help()
constexpr std::u8string_view CAPTURES_PATH = u8"resources\\captures\\";

class image_helper
{
public:
	void save_to_local(const std::filesystem::path& _save_path, uint32_t _width, uint32_t _height, uint32_t _channel = 4, OIIO::TypeDesc _format = OIIO::TypeDesc::UINT8, void* _data = nullptr);
	static image_helper& get_image_help() noexcept;

private:
	image_helper() = default;
	~image_helper() = default;
	image_helper(const image_helper&) = delete;
	image_helper& operator=(const image_helper&) = delete;
	image_helper(const image_helper&&) = delete;
	image_helper& operator=(const image_helper&&) = delete;

	static image_helper helper;
};