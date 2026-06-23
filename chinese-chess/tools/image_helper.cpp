#include "image_helper.h"

image_helper image_helper::helper;

void image_helper::read_hdr_image(const std::filesystem::path& _hdr_path, uint32_t& _width, uint32_t& _height, std::vector<float>& _hdr_data)
{
	auto image = OIIO::ImageInput::open(_hdr_path);
	if (!image)
	{
		throw std::runtime_error(std::format("Failed to open image {}.", _hdr_path.string()));
	}

	const OIIO::ImageSpec& spec = image->spec();
	_width = spec.width;
	_height = spec.height;

	_hdr_data.resize(static_cast<size_t>(_width) * _height * 4, 1.f);
	if (!image->read_image(0, 0, 0, 4, OIIO::TypeDesc::FLOAT, _hdr_data.data(), 4 * sizeof(float), 4 * sizeof(float) * _width, OIIO::AutoStride))
	{
		throw std::runtime_error(std::format("Failed to read image {}.", _hdr_path.string()));
	}
}

void image_helper::save_to_local(const std::filesystem::path& _save_path, uint32_t _width, uint32_t _height, uint32_t _channel, OIIO::TypeDesc _format, void* _data)
{
	auto write_image = OIIO::ImageOutput::create(_save_path);
	if (!write_image->open(_save_path, OIIO::ImageSpec(_width, _height, _channel, _format)))
	{
		throw std::runtime_error(std::format("Can not open file, please check path: {}", _save_path.generic_string()));
	}
	if (!write_image->write_image(_format, _data))
	{
		throw std::runtime_error("Can not write file!");
	}
	write_image->close();
}

image_helper& image_helper::get_image_help() noexcept
{
	return helper;
}
