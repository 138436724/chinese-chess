#pragma once

#include <cstdint>
#include <filesystem>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/typedesc.h>
#include <string_view>

#define IMAGE_HELPER image_helper::get_image_help()
constexpr std::u8string_view CAPTURES_PATH = u8"resources\\captures\\";
constexpr std::u8string_view TEXTURES_PATH = u8"resources\\textures\\";

template <typename T>
	requires (std::is_arithmetic_v<T>)
struct image_info
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t channels = 0;
	std::vector<T> buffer;
};

class image_helper
{
public:
	template <typename T>
		requires (std::is_arithmetic_v<T>)
	image_info<T> read_image(const std::filesystem::path& _image_path, std::optional<uint32_t> _use_channels = std::nullopt)
	{
		auto image = OIIO::ImageInput::open(_image_path);
		if (!image)
		{
			throw std::runtime_error(std::format("Failed to open image {}.", _image_path.string()));
		}

		const OIIO::ImageSpec& spec = image->spec();
		image_info<T> info{
			.width = static_cast<uint32_t>(spec.width),
			.height = static_cast<uint32_t>(spec.height),
			.channels = _use_channels.value_or(static_cast<uint32_t>(spec.nchannels)),
		};

		auto desc = OIIO::TypeDesc::UNKNOWN;
		if constexpr (std::is_same_v<T, uint8_t>)
		{
			desc = OIIO::TypeDesc::UINT8;
		}
		else if constexpr (std::is_same_v<T, int8_t>)
		{
			desc = OIIO::TypeDesc::INT8;
		}
		else if constexpr (std::is_same_v<T, uint16_t>)
		{
			desc = OIIO::TypeDesc::UINT16;
		}
		else if constexpr (std::is_same_v<T, int16_t>)
		{
			desc = OIIO::TypeDesc::INT16;
		}
		else if constexpr (std::is_same_v<T, uint32_t>)
		{
			desc = OIIO::TypeDesc::UINT32;
		}
		else if constexpr (std::is_same_v<T, int32_t>)
		{
			desc = OIIO::TypeDesc::INT32;
		}
		else if constexpr (std::is_same_v<T, float>)
		{
			desc = OIIO::TypeDesc::FLOAT;
		}

		info.buffer.resize(static_cast<size_t>(info.width) * info.height * info.channels, static_cast<T>(0));
		if (!image->read_image(0, 0, 0, info.channels, desc, info.buffer.data()/*, sizeof(T) * info.channels, sizeof(T) * info.channels * info.width, OIIO::AutoStride*/))
		{
			throw std::runtime_error(std::format("Failed to read image {}.", _image_path.string()));
		}

		return info;
	}

	void read_hdr_image(const std::filesystem::path& _hdr_path, uint32_t& _width, uint32_t& _height, std::vector<float>& _hdr_data);
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