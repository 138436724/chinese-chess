#pragma once

#include <filesystem>
#include <OpenColorIO/OpenColorIO.h>
#include <OpenColorIO/OpenColorTypes.h>
#include <string_view>
#include <vector>

#define OCIO_HELPER ocio_helper::get_ocio_helper()
constexpr std::u8string_view OCIOS_PATH = u8"resources\\ocios\\";
constexpr std::u8string_view OCIO_FUNCTION_NAME = u8"ocio_conversion";

namespace OCIO = OCIO_NAMESPACE;

class ocio_helper
{
public:
	std::vector<char> replace_and_compile(OCIO::GpuShaderDescRcPtr& _shader_desc, const std::filesystem::path& _shader_path, const std::vector<std::string>& _entry_name) const;
	OCIO::GpuShaderDescRcPtr generate_shader_info(const std::filesystem::path& _ocio_path) const noexcept;
	void copy_uniform_to_buffer(OCIO::GpuShaderDescRcPtr& _shader_desc, void* _buffer_address) const noexcept;

	static ocio_helper& get_ocio_helper() noexcept;

private:
	ocio_helper() = default;
	~ocio_helper() = default;

	ocio_helper(const ocio_helper&) = delete;
	ocio_helper& operator=(const ocio_helper&) = delete;
	ocio_helper(const ocio_helper&&) = delete;
	ocio_helper& operator=(const ocio_helper&&) = delete;

	static ocio_helper helper;
};