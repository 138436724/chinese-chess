#pragma once

#include <array>
#include <expected>
#include <filesystem>
#include <slang-com-ptr.h>
#include <slang.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#define SHADER_COMPILER shader_compiler::get_shader_compiler()

constexpr std::string_view SHADERS_PATH = "shaders\\";

class shader_compiler
{
public:
    [[nodiscard]] std::expected<std::vector<char>, std::string> compile_shader_to_spv(const std::filesystem::path& _shader_path,
                                                                                      const std::vector<std::string_view>& _entry_name) const;
    [[nodiscard]] std::expected<std::vector<char>, std::string> compile_shader_to_spv(const std::string& _shader_string,
                                                                                      const std::vector<std::string_view>& _entry_name) const;
    [[nodiscard]] static shader_compiler& get_shader_compiler() noexcept;

private:
    shader_compiler();
    ~shader_compiler();
    shader_compiler(const shader_compiler&)            = delete;
    shader_compiler& operator=(const shader_compiler&) = delete;
    shader_compiler(shader_compiler&&)                 = delete;
    shader_compiler& operator=(shader_compiler&&)      = delete;

    void load_cache();
    void save_cache() const;

    static shader_compiler               compiler;
    Slang::ComPtr<slang::IGlobalSession> global_session;

    std::vector<slang::CompilerOptionEntry> options;
    slang::TargetDesc                       target_desc;
    slang::SessionDesc                      session_desc;

    std::string shader_cache_header;

    struct shader_cache_info
    {
        std::string       dependence;
        std::string       fingerprint;
        std::vector<char> spirv;
    };
    mutable std::unordered_map<std::string, shader_cache_info> shader_cache;
};
