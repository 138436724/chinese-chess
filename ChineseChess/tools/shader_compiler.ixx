export module shader_compiler;

import <slang.h>;
import <slang-com-ptr.h>;
import std;

export constexpr std::string_view SHADERS_PATH = "resources\\shaders\\";

export class shader_compiler
{
public:
	const std::vector<char> compile_shader_to_spv(const std::filesystem::path& _shader_path, const std::vector<std::string>& _entry_name);
	static shader_compiler& get_shader_compiler() noexcept;

private:
	shader_compiler();
	~shader_compiler();
	shader_compiler(const shader_compiler&) = delete;
	shader_compiler& operator=(const shader_compiler&) = delete;
	shader_compiler(const shader_compiler&&) = delete;
	shader_compiler& operator=(const shader_compiler&&) = delete;

	bool compile_slang_to_spv(const std::filesystem::path& _shader_path, const std::vector<std::string>& _entry_name, slang::IBlob** _spirv_code);

	static shader_compiler compiler;
	Slang::ComPtr<slang::IGlobalSession> globalSession;
};
