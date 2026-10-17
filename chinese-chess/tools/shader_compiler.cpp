#include "shader_compiler.h"

#include "file_watcher.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <ranges>
#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

shader_compiler shader_compiler::compiler;

namespace {

constexpr std::string_view shader_cache_path  = "resources\\cache\\shader_cache.cache";
constexpr std::string_view shader_cache_magic = "shader_cache_magic";
constexpr char             split_char         = '\n';

[[nodiscard]] inline std::streamoff get_file_size(std::ifstream& _file)
{
    _file.seekg(0, std::ios::end);
    const std::streamoff file_size = static_cast<std::streamoff>(_file.tellg());
    _file.seekg(0, std::ios::beg);
    return file_size;
}

template <typename cache_blob>
    requires(std::same_as<cache_blob, std::string> || std::same_as<cache_blob, std::vector<char>>)
[[nodiscard]] bool read_cache_blob(std::ifstream& _file, cache_blob& _blob)
{
    size_t length = 0;
    if (!_file.read(reinterpret_cast<char*>(&length), sizeof(length)))
    {
        return false;
    }

    if (length == 0)
    {
        return true;
    }

    _blob.resize(length);
    return static_cast<bool>(_file.read(_blob.data(), static_cast<std::streamsize>(length)));
}

inline void write_cache_blob(std::ofstream& _file, const std::span<const char> _blob)
{
    if (!_blob.empty())
    {
        const size_t length = _blob.size();
        _file.write(reinterpret_cast<const char*>(&length), sizeof(length));
        _file.write(_blob.data(), static_cast<std::streamsize>(_blob.size()));
    }
}

[[nodiscard]] std::string generate_cache_header(const std::vector<slang::CompilerOptionEntry>& _options,
                                                const std::string_view                         _build_tag,
                                                const slang::TargetDesc&                       _target_desc)
{
#ifndef NDEBUG
    const auto result =
        std::format("{}{}{}{}{}{}{}{}{}", shader_cache_magic, split_char, _build_tag, split_char, "DEBUG", split_char,
                    std::to_underlying(_target_desc.format), split_char, std::to_underlying(_target_desc.profile));
#else
    const auto result =
        std::format("{}{}{}{}{}{}{}{}{}", shader_cache_magic, split_char, _build_tag, split_char, "RELEASE", split_char,
                    std::to_underlying(_target_desc.format), split_char, std::to_underlying(_target_desc.profile));
#endif  // !NDEBUG

    const auto option_string =
        _options | std::views::transform([](const auto& option) {
            return std::format("{}{}{}{}{}{}{}", split_char, std::to_underlying(option.name),
                               std::to_underlying(option.value.kind), option.value.intValue0, option.value.intValue1,
                               option.value.stringValue0 == nullptr ? "" : option.value.stringValue0,
                               option.value.stringValue1 == nullptr ? "" : option.value.stringValue1);
        })
        | std::views::join | std::ranges::to<std::string>();

    return result + option_string;
}

constinit int global_counter = 0;

[[nodiscard]] std::string get_diagnostics(const Slang::ComPtr<slang::IBlob>& _diagnostic_blob) noexcept
{
    if (_diagnostic_blob == nullptr)
    {
        return {};
    }
    return std::string(static_cast<const char*>(_diagnostic_blob->getBufferPointer()), _diagnostic_blob->getBufferSize());
}

#ifndef NDEBUG
void diagnose_if_needed(const Slang::ComPtr<slang::IBlob>& _diagnostic_blob) noexcept
{
    if (_diagnostic_blob != nullptr)
    {
        std::println("{}", get_diagnostics(_diagnostic_blob));
    }
}

void print_entrypoint_hashes(int _entrypoint_count, int _target_count, const Slang::ComPtr<slang::IComponentType>& _composed_program)
{
    std::ranges::for_each(std::views::iota(0, _target_count), [&](int target_index) {
        std::ranges::for_each(std::views::iota(0, _entrypoint_count), [&](int entrypoint_index) {
            Slang::ComPtr<slang::IBlob> entrypoint_hash_blob;
            _composed_program->getEntryPointHash(entrypoint_index, target_index, entrypoint_hash_blob.writeRef());

            const auto hash_str =
                std::span<const uint8_t>(static_cast<const uint8_t*>(entrypoint_hash_blob->getBufferPointer()),
                                         entrypoint_hash_blob->getBufferSize())
                | std::views::transform([](const auto& num) static { return std::format("{:02X}", num); })
                | std::views::join | std::ranges::to<std::string>();

            std::println("callIdx: {:2}, entrypoint: {:2}, target: {:2}, hash: {}", global_counter, entrypoint_index,
                         target_index, hash_str);
            global_counter++;
        });
    });
}
#else
void diagnose_if_needed(const Slang::ComPtr<slang::IBlob>&) noexcept {}
void print_entrypoint_hashes(int, int, const Slang::ComPtr<slang::IComponentType>&) {}
#endif  // !NDEBUG

[[nodiscard]] std::expected<Slang::ComPtr<slang::IBlob>, std::string> slang_module_to_spv(Slang::ComPtr<slang::ISession>& _session,
                                                                                          Slang::ComPtr<slang::IBlob>& _diagnostics_blob,
                                                                                          Slang::ComPtr<slang::IModule>& _slang_module,
                                                                                          const std::vector<std::string_view>& _entry_name)
{
    std::vector<slang::IComponentType*> component_types;
    for (const auto& entry_name : _entry_name)
    {
        Slang::ComPtr<slang::IEntryPoint> entry_point;
        _slang_module->findEntryPointByName(entry_name.data(), entry_point.writeRef());
        if (!entry_point)
        {
            return std::unexpected(std::format("Entry point \"{}\" not found in shader", entry_name));
        }
        component_types.emplace_back(entry_point);
    }

    Slang::ComPtr<slang::IBlob>          spirv_code;
    Slang::ComPtr<slang::IComponentType> composed_program;
    auto result = _session->createCompositeComponentType(component_types.data(), component_types.size(),
                                                         composed_program.writeRef(), _diagnostics_blob.writeRef());
    diagnose_if_needed(_diagnostics_blob);
    if (!SLANG_SUCCEEDED(result))
    {
        return std::unexpected(get_diagnostics(_diagnostics_blob));
    }

    Slang::ComPtr<slang::IComponentType> linked_program;
    result = composed_program->link(linked_program.writeRef(), _diagnostics_blob.writeRef());
    diagnose_if_needed(_diagnostics_blob);
    if (!SLANG_SUCCEEDED(result))
    {
        return std::unexpected(get_diagnostics(_diagnostics_blob));
    }

    result = linked_program->getTargetCode(0, spirv_code.writeRef(), _diagnostics_blob.writeRef());
    diagnose_if_needed(_diagnostics_blob);
    if (!SLANG_SUCCEEDED(result))
    {
        return std::unexpected(get_diagnostics(_diagnostics_blob));
    }

    print_entrypoint_hashes(static_cast<int>(_entry_name.size()), 1, composed_program);

    return spirv_code;
}

[[nodiscard]] std::pair<std::string, std::string> get_dependencies_and_fingerprint(const Slang::ComPtr<slang::IModule>& _module)
{
    std::string              dependence;
    std::vector<std::string> hashes;

    const SlangInt32 dependency_count = _module->getDependencyFileCount();
    for (SlangInt32 i = 0; i < dependency_count; ++i)
    {
        const char* const dependency_path = _module->getDependencyFilePath(i);  // include the slang file
        if (dependency_path == nullptr)
        {
            continue;
        }

        if (std::error_code ec; !std::filesystem::exists(dependency_path, ec) || ec)
        {
            return {};
        }

        auto dependency_hash = FILE_WATCHER.generate_file_hash(dependency_path);
        if (!dependency_hash)
        {
            return {};
        }
        dependence += dependency_path;
        dependence += split_char;
        hashes.push_back(std::move(*dependency_hash));
    }

    std::ranges::sort(hashes);
    return {std::move(dependence), hashes | std::views::join_with(split_char) | std::ranges::to<std::string>()};
}

[[nodiscard]] bool verify_dependencies(std::string_view _dependence, std::string_view _fingerprint)
{
    std::vector<std::string> hashes;
    for (const auto& line : _dependence | std::views::split(split_char))
    {
        if (line.empty())
        {
            continue;
        }

        auto current_hash = FILE_WATCHER.generate_file_hash(std::filesystem::path(line.begin(), line.end()));
        if (!current_hash)
        {
            return false;
        }

        hashes.push_back(std::move(*current_hash));
    }

    if (hashes.empty())
    {
        return false;
    }

    std::ranges::sort(hashes);
    return (hashes | std::views::join_with(split_char) | std::ranges::to<std::string>()) == _fingerprint;
}

[[nodiscard]] std::string generate_cache_key(std::string_view _shader_path, const std::vector<std::string_view>& _entry_names)
{
    return std::string(_shader_path) + split_char
           + (_entry_names | std::views::join_with(split_char) | std::ranges::to<std::string>());
}

[[nodiscard]] std::string_view get_path_from_cache_key(std::string_view _cache_key) noexcept
{
    return _cache_key.substr(0, _cache_key.find(split_char));
}

}  // namespace

shader_compiler::shader_compiler()
{
    const auto result = slang::createGlobalSession(global_session.writeRef());
    if (!SLANG_SUCCEEDED(result))
    {
        throw std::runtime_error("Cannot create global session.");
    }

    options = {
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::EmitSpirvDirectly,
            {slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
        },
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::VulkanUseEntryPointName,
            {slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
        },
#ifndef NDEBUG
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::Optimization,
            {slang::CompilerOptionValueKind::Int, SlangOptimizationLevel::SLANG_OPTIMIZATION_LEVEL_NONE, 0, nullptr, nullptr},
        },
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::DebugInformation,
            {slang::CompilerOptionValueKind::Int, SlangDebugInfoLevel::SLANG_DEBUG_INFO_LEVEL_MAXIMAL, 0, nullptr, nullptr},
        },
#else
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::Optimization,
            {slang::CompilerOptionValueKind::Int, SlangOptimizationLevel::SLANG_OPTIMIZATION_LEVEL_MAXIMAL, 0, nullptr, nullptr},
        },
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::DebugInformation,
            {slang::CompilerOptionValueKind::Int, SlangDebugInfoLevel::SLANG_DEBUG_INFO_LEVEL_NONE, 0, nullptr, nullptr},
        },
#endif  // !NDEBUG
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::MatrixLayoutColumn,
            {slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
        },
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::Capability,
            {slang::CompilerOptionValueKind::Int, global_session->findCapability("spvRayQueryKHR"), 0, nullptr, nullptr},
        },
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::ForceCLayout,
            {slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
        },
    };

    target_desc.format  = SLANG_SPIRV;
    target_desc.profile = global_session->findProfile("spirv_1_6");

    session_desc.targets                  = &target_desc;
    session_desc.targetCount              = 1;
    session_desc.compilerOptionEntries    = options.data();
    session_desc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());

    shader_cache_header = generate_cache_header(options, global_session->getBuildTagString(), target_desc);

    load_cache();
}

shader_compiler::~shader_compiler()
{
    save_cache();
}

std::expected<std::vector<char>, std::string> shader_compiler::compile_shader_to_spv(const std::filesystem::path& _shader_path,
                                                                                     const std::vector<std::string_view>& _entry_name) const
{
    const std::string shader_path = _shader_path.generic_string();
    const std::string cache_key   = generate_cache_key(shader_path, _entry_name);

    if (const auto iter = shader_cache.find(cache_key); iter != shader_cache.end() && !iter->second.dependence.empty()
                                                        && verify_dependencies(iter->second.dependence, iter->second.fingerprint))
    {
        return iter->second.spirv;
    }

    auto              desc         = session_desc;
    const std::string parent_path  = _shader_path.parent_path().generic_string();
    const std::array  search_paths = {parent_path.c_str()};
    desc.searchPathCount           = static_cast<int64_t>(search_paths.size());
    desc.searchPaths               = search_paths.data();

    Slang::ComPtr<slang::ISession> session;
    if (!SLANG_SUCCEEDED(global_session->createSession(desc, session.writeRef())))
    {
        return std::unexpected("Failed to create Slang session");
    }

    const std::string             module_name = _shader_path.stem().generic_string();
    Slang::ComPtr<slang::IBlob>   diagnostics_blob;
    Slang::ComPtr<slang::IModule> slang_module =
        Slang::ComPtr<slang::IModule>(session->loadModule(module_name.c_str(), diagnostics_blob.writeRef()));

    diagnose_if_needed(diagnostics_blob);
    if (!slang_module)
    {
        return std::unexpected(get_diagnostics(diagnostics_blob).empty() ? "Failed to load shader module" :
                                                                           get_diagnostics(diagnostics_blob));
    }

    auto [dependence, fingerprint] = get_dependencies_and_fingerprint(slang_module);

    auto spirv_code = slang_module_to_spv(session, diagnostics_blob, slang_module, _entry_name);
    if (!spirv_code)
    {
        return std::unexpected(std::format("Failed to compile shader {}: {}", shader_path, spirv_code.error()));
    }

    const auto*       ptr      = static_cast<const char*>((*spirv_code)->getBufferPointer());
    const auto        byte_num = (*spirv_code)->getBufferSize();
    std::vector<char> spirv(ptr, ptr + byte_num);

    if (!dependence.empty())
    {
        shader_cache.insert_or_assign(cache_key, shader_cache_info{std::move(dependence), std::move(fingerprint), spirv});
    }

    return spirv;
}

std::expected<std::vector<char>, std::string> shader_compiler::compile_shader_to_spv(const std::string& _shader_string,
                                                                                     const std::vector<std::string_view>& _entry_name) const
{
    Slang::ComPtr<slang::ISession> session;
    if (!SLANG_SUCCEEDED(global_session->createSession(session_desc, session.writeRef())))
    {
        return std::unexpected("Failed to create Slang session");
    }

    Slang::ComPtr<slang::IBlob>   diagnostics_blob;
    Slang::ComPtr<slang::IModule> slang_module = Slang::ComPtr<slang::IModule>(
        session->loadModuleFromSourceString("shaders", "memory:shader", _shader_string.c_str(), diagnostics_blob.writeRef()));

    diagnose_if_needed(diagnostics_blob);
    if (!slang_module)
    {
        return std::unexpected(get_diagnostics(diagnostics_blob).empty() ? "Failed to load shader module" :
                                                                           get_diagnostics(diagnostics_blob));
    }

    auto spirv_code = slang_module_to_spv(session, diagnostics_blob, slang_module, _entry_name);
    if (!spirv_code)
    {
        return std::unexpected(std::format("Failed to compile shader string: {}", spirv_code.error()));
    }

    const auto* ptr      = static_cast<const char*>((*spirv_code)->getBufferPointer());
    const auto  byte_num = (*spirv_code)->getBufferSize();
    return std::vector<char>(ptr, ptr + byte_num);
}

shader_compiler& shader_compiler::get_shader_compiler() noexcept
{
    return compiler;
}

void shader_compiler::load_cache()
{
    std::ifstream cache_file(shader_cache_path.data(), std::ios::binary);
    if (!cache_file.is_open())
    {
        return;
    }

    const std::streamoff file_size = get_file_size(cache_file);
    if (file_size <= 0)
    {
        return;
    }

    try
    {
        if (std::string header; !read_cache_blob(cache_file, header) || header != shader_cache_header)
        {
            return;
        }

        size_t cache_count = 0;
        if (!cache_file.read(reinterpret_cast<char*>(&cache_count), sizeof(cache_count)))
        {
            return;
        }

        for (size_t i = 0; i < cache_count; ++i)
        {
            std::string       name;
            std::string       dependence;
            std::string       fingerprint;
            std::vector<char> spirv;

            if (!read_cache_blob(cache_file, name) || !read_cache_blob(cache_file, dependence)
                || !read_cache_blob(cache_file, fingerprint) || !read_cache_blob(cache_file, spirv))
            {
                return;
            }

            shader_cache.emplace(std::move(name),
                                 shader_cache_info{std::move(dependence), std::move(fingerprint), std::move(spirv)});
        }
    }
    catch (const std::exception&)
    {
    }
}

void shader_compiler::save_cache() const
{
    std::erase_if(shader_cache,
                  [](const auto& item) static { return !std::filesystem::exists(get_path_from_cache_key(item.first)); });

    std::filesystem::create_directories(std::filesystem::path(shader_cache_path).parent_path());
    std::ofstream cache_file(std::filesystem::path(shader_cache_path), std::ios::binary | std::ios::trunc);
    if (!cache_file.is_open())
    {
        return;
    }

    write_cache_blob(cache_file, shader_cache_header);

    const size_t shader_count = shader_cache.size();
    cache_file.write(reinterpret_cast<const char*>(&shader_count), sizeof(shader_count));

    for (const auto& [name, info] : shader_cache)
    {
        write_cache_blob(cache_file, name);
        write_cache_blob(cache_file, info.dependence);
        write_cache_blob(cache_file, info.fingerprint);
        write_cache_blob(cache_file, info.spirv);
    }
}
