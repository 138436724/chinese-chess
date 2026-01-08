module image_help;

image_help image_help::helper;

bool image_help::save_to_local(const std::filesystem::path& _save_path, uint32_t _width, uint32_t _height, uint32_t _channel, OpenImageIO_v3_0::TypeDesc _format, void* _data)
{
	try
	{
		auto write_image = OpenImageIO_v3_0::ImageOutput::create(_save_path.generic_string());
		write_image->open(_save_path.generic_string(), OpenImageIO_v3_0::ImageSpec(_width, _height, _channel, _format));
		write_image->write_image(_format, _data);
		write_image->close();
	}
	catch (const std::exception&)
	{
		return false;
	}

	return true;
}

image_help& image_help::get_image_help() noexcept
{
	return helper;
}
