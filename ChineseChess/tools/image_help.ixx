export module image_help;

import <OpenImageIO/imageio.h>;
import <cstdint>;
import std;
import vulkan_hpp;

export constexpr std::string_view CAPTURES_PATH = "resources\\captures\\";

export class image_help
{
public:
	bool save_to_local(const std::filesystem::path& _save_path, uint32_t _width, uint32_t _height, uint32_t _channel = 4, OpenImageIO_v3_0::TypeDesc _format = OpenImageIO_v3_0::TypeDesc::UINT8, void* _data = nullptr);
	static image_help& get_image_help() noexcept;

private:
	image_help() = default;
	~image_help() = default;
	image_help(const image_help&) = delete;
	image_help& operator=(const image_help&) = delete;
	image_help(const image_help&&) = delete;
	image_help& operator=(const image_help&&) = delete;

	static image_help helper;
};