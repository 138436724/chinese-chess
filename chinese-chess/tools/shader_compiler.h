#pragma once

#include <filesystem>
#include <slang.h>
#include <slang-com-ptr.h>
#include <string>
#include <vector>

#define SHADER_COMPILER shader_compiler::get_shader_compiler()
constexpr std::string_view VERT_ENTYR_NAME = "vertMain";
constexpr std::string_view FRAG_ENTYR_NAME = "fragMain";
constexpr std::u8string_view SHADERS_PATH = u8"resources\\shaders\\";

class shader_compiler
{
public:
	std::vector<char> compile_shader_to_spv(const std::filesystem::path& _shader_path, const std::vector<std::string>& _entry_name) noexcept;
	static shader_compiler& get_shader_compiler() noexcept;

private:
	shader_compiler();
	~shader_compiler();
	shader_compiler(const shader_compiler&) = delete;
	shader_compiler& operator=(const shader_compiler&) = delete;
	shader_compiler(const shader_compiler&&) = delete;
	shader_compiler& operator=(const shader_compiler&&) = delete;

	bool compile_slang_to_spv(const std::filesystem::path& _shader_path, const std::vector<std::string>& _entry_name, slang::IBlob** _spirv_code) noexcept;

	static shader_compiler compiler;
	Slang::ComPtr<slang::IGlobalSession> globalSession;
};