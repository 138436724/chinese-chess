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

constexpr std::string_view VERT_ENTRY_NAME               = "vertMain";
constexpr std::string_view FRAG_ENTRY_NAME               = "fragMain";
constexpr std::string_view RAY_GEN_ENTRY_NAME            = "rayGenMain";
constexpr std::string_view RAY_MISS_ENTRY_NAME           = "rayMissMain";
constexpr std::string_view RAY_SHADOW_MISS_ENTRY_NAME    = "rayShadowMissMain";
constexpr std::string_view RAY_CLOSEST_HIT_ENTRY_NAME    = "rayClosestHitMain";
constexpr std::string_view RAY_SHADOW_ANY_HIT_ENTRY_NAME = "rayShadowAnyHitMain";
constexpr std::string_view SHADERS_PATH                  = "resources\\shaders\\";

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

    static shader_compiler               compiler;
    Slang::ComPtr<slang::IGlobalSession> global_session;

    std::array<slang::CompilerOptionEntry, 4> options;
    slang::TargetDesc                         target_desc;
    slang::SessionDesc                        session_desc;

    struct shader_cache_info
    {
        std::string       dependence;
        std::string       fingerprint;
        std::vector<char> spirv;
    };
    mutable std::unordered_map<std::string, shader_cache_info> shader_cache;
};
