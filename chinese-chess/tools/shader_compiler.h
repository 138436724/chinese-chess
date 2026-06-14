#pragma once

#include <array>
#include <filesystem>
#include <slang.h>
#include <slang-com-ptr.h>
#include <string>
#include <vector>

#define SHADER_COMPILER shader_compiler::get_shader_compiler()
constexpr std::string_view VERT_ENTYR_NAME = "vertMain";
constexpr std::string_view FRAG_ENTYR_NAME = "fragMain";
constexpr std::string_view RAY_GEN_ENTYR_NAME = "rayGenMain";
constexpr std::string_view RAY_MISS_ENTYR_NAME = "rayMissMain";
constexpr std::string_view RAY_SHADOW_MISS_ENTYR_NAME = "rayShadowMissMain";
constexpr std::string_view RAY_CLOSEST_HIT_ENTRY_NAME = "rayClosestHitMain";
constexpr std::string_view RAY_SHADOW_ANY_HIT_ENTYR_NAME = "rayShadowAnyHitMain";
constexpr std::u8string_view SHADERS_PATH = u8"resources\\shaders\\";

class shader_compiler
{
public:
	std::vector<char> compile_shader_to_spv(const std::filesystem::path& _shader_path, const std::vector<std::string_view>& _entry_name) noexcept;
	std::vector<char> compile_shader_to_spv(const std::string& _shader_string, const std::vector<std::string_view>& _entry_name) noexcept;
	static shader_compiler& get_shader_compiler() noexcept;

private:
	shader_compiler();
	~shader_compiler() = default;
	shader_compiler(const shader_compiler&) = delete;
	shader_compiler& operator=(const shader_compiler&) = delete;
	shader_compiler(const shader_compiler&&) = delete;
	shader_compiler& operator=(const shader_compiler&&) = delete;

	static void diagnose_if_needed(const Slang::ComPtr<slang::IBlob>& _diagnostic_blob) noexcept;
	void print_entrypoint_hashes(int _entrypoint_count, int _target_count, const Slang::ComPtr<slang::IComponentType>& _composed_program) noexcept;

	bool slang_to_slang_module(const slang::SessionDesc& _session_desc, const std::string& _shader_string, bool _as_shader_name, const std::vector<std::string_view>& _entry_name, slang::IBlob** _spirv_code) noexcept;
	bool slang_module_to_spv(Slang::ComPtr<slang::ISession>& _session, Slang::ComPtr<slang::IBlob>& _diagnostics_blob, Slang::ComPtr<slang::IModule>& _slang_module, const std::vector<std::string_view>& _entry_name, slang::IBlob** _spirv_code) noexcept;

	static shader_compiler compiler;
	Slang::ComPtr<slang::IGlobalSession> global_session;

	std::array<slang::CompilerOptionEntry, 4> options;
	slang::TargetDesc target_desc;
	slang::SessionDesc session_desc;

	int global_counter = 0;
};