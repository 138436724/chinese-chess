module image_help;

image_help image_help::helper;

void image_help::save_to_local(const std::filesystem::path& _save_path, uint32_t _width, uint32_t _height, uint32_t _channel, OIIO::TypeDesc _format, void* _data)
{
	auto write_image = OIIO::ImageOutput::create(_save_path.generic_string());
	if (!write_image->open(_save_path.generic_string(), OIIO::ImageSpec(_width, _height, _channel, _format)))
	{
		throw std::runtime_error(std::format("Can not open file, please check path: {}", _save_path.generic_string()));
	}
	if (!write_image->write_image(_format, _data))
	{
		throw std::runtime_error("Can not write file!");
	}
	write_image->close();
}

image_help& image_help::get_image_help() noexcept
{
	return helper;
}
