#include "ocio_helper.h"
#include "shader_compiler.h"
#include "string_helper.h"
#include <algorithm>
#include <ranges>

ocio_helper ocio_helper::helper;

std::vector<char> ocio_helper::replace_and_compile(OCIO::GpuShaderDescRcPtr& _shader_desc, const std::filesystem::path& _shader_path, const std::vector<std::string_view>& _entry_name) const
{
	auto ocio_function_name = string_helper::convert_to<std::string, std::u8string>(u8"float4 " + std::u8string(OCIO_FUNCTION_NAME));

	std::string shader_string = _shader_desc->getShaderText();

	// replace generate ocio function in ocio_helper.slang
	std::ifstream shader_file(_shader_path);
	if (!shader_file)
	{
		throw std::runtime_error("Can not open ocio slang file!");
	}

	std::string original_string((std::istreambuf_iterator<char>(shader_file)), std::istreambuf_iterator<char>());
	shader_file.close();

	std::string final_string = original_string
		| std::views::split('\n')
		| std::views::transform([&](const auto& _line)
			{
				std::string s(_line.begin(), _line.end());
				if (s.contains(ocio_function_name)) [[unlikely]]
				{
					return shader_string;
				}
				return s;
			})
		| std::views::join_with('\n')
		| std::ranges::to<std::string>();

	return SHADER_COMPILER.compile_shader_to_spv(final_string, _entry_name);
}

OCIO::GpuShaderDescRcPtr ocio_helper::generate_shader_info(const std::filesystem::path& _ocio_path) const noexcept
{
	auto ocio_function_name = string_helper::convert_to<std::string, std::u8string>(std::u8string(OCIO_FUNCTION_NAME));

	auto config = OCIO::Config::CreateFromFile(_ocio_path.string().c_str());

	const char* display = config->getDefaultDisplay();
	const char* view = config->getDefaultView(display);

	auto transform = OCIO::DisplayViewTransform::Create();
	transform->setSrc(OCIO::ROLE_SCENE_LINEAR);
	transform->setDisplay(display);
	transform->setView(view);

	auto processor = config->getProcessor(transform);
	auto gpu_processor = processor->getDefaultGPUProcessor();

	auto shader_desc = OCIO::GpuShaderDesc::CreateShaderDesc();
	shader_desc->setLanguage(OCIO::GPU_LANGUAGE_HLSL_SM_5_0/*GPU_LANGUAGE_GLSL_VK_4_6*/); // hlsl and slang are very similar 
	shader_desc->setFunctionName(ocio_function_name.c_str());
	shader_desc->setResourcePrefix("ocio_");

	gpu_processor->extractGpuShaderInfo(shader_desc);

	return shader_desc;
}

void ocio_helper::copy_uniform_to_buffer(OCIO::GpuShaderDescRcPtr& _shader_desc, void* _buffer_address) const noexcept
{
	const auto num_uniforms = _shader_desc->getNumUniforms();
	if (num_uniforms <= 0)
	{
		return;
	}

	if (_shader_desc->getUniformBufferSize() <= 0)
	{
		return;
	}

	std::ranges::for_each(std::views::iota(0u, num_uniforms), [&](const uint32_t i) {
		OCIO::GpuShaderDesc::UniformData uniform_data;
		//const auto name = _shader_desc->getUniform(i, uniform_data);

		// todo slang 是尽量紧密排列的，可能需要实时计算一下是否是16的倍数来决定能否写入或者另起一行
		char* dest = static_cast<char*>(_buffer_address) + uniform_data.m_bufferOffset;
		if (uniform_data.m_getDouble)
		{
			const float val = static_cast<float>(uniform_data.m_getDouble());
			memcpy(dest, &val, sizeof(float));
		}
		else if (uniform_data.m_getBool)
		{
			const int val = uniform_data.m_getBool() ? 1 : 0;
			memcpy(dest, &val, sizeof(int));
		}
		else if (uniform_data.m_getFloat3)
		{
			// vec3 in std140: write 3 floats (12 bytes), padded to 16 bytes
			const auto vals = uniform_data.m_getFloat3();
			memcpy(dest, vals.data(), 3 * sizeof(float));
		}
		else if (uniform_data.m_vectorFloat.m_getSize && uniform_data.m_vectorFloat.m_getVector)
		{
			// In std140, each array element is padded to 16 bytes
			const float* vals = uniform_data.m_vectorFloat.m_getVector();
			const size_t count = uniform_data.m_vectorFloat.m_getSize();
			for (size_t j = 0; j < count; ++j)
			{
				memcpy(dest + j * 16, &vals[j], sizeof(float));
			}
		}
		else if (uniform_data.m_vectorInt.m_getSize && uniform_data.m_vectorInt.m_getVector)
		{
			// In std140, each array element is padded to 16 bytes
			const int* vals = uniform_data.m_vectorInt.m_getVector();
			const size_t count = uniform_data.m_vectorInt.m_getSize();
			for (size_t j = 0; j < count; ++j)
			{
				memcpy(dest + j * 16, &vals[j], sizeof(int));
			}
		}
		});
}

ocio_helper& ocio_helper::get_ocio_helper() noexcept
{
	return helper;
}
