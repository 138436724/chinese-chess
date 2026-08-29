#include "shader_compiler.h"

#include "file_watcher.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <print>
#include <ranges>
#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <system_error>
#include <utility>

shader_compiler shader_compiler::compiler;

namespace {

constexpr std::string_view shader_cache_path   = "resources\\cache\\shader_cache.cache";
constexpr std::string_view shader_cache_header = "shader_cache_header";
constexpr char             split_char          = '\n';

int global_counter = 0;

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

            std::println("callIdx: {}, entrypoint: {}, target: {}, hash: {}", global_counter, entrypoint_index, target_index, hash_str);
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
    if (!SLANG_SUCCEEDED(result))
    {
        return std::unexpected(get_diagnostics(_diagnostics_blob));
    }
    else
    {
        diagnose_if_needed(_diagnostics_blob);
    }

    Slang::ComPtr<slang::IComponentType> linked_program;
    result = composed_program->link(linked_program.writeRef(), _diagnostics_blob.writeRef());
    if (!SLANG_SUCCEEDED(result))
    {
        return std::unexpected(get_diagnostics(_diagnostics_blob));
    }
    else
    {
        diagnose_if_needed(_diagnostics_blob);
    }

    result = linked_program->getTargetCode(0, spirv_code.writeRef(), _diagnostics_blob.writeRef());
    if (!SLANG_SUCCEEDED(result))
    {
        return std::unexpected(get_diagnostics(_diagnostics_blob));
    }
    else
    {
        diagnose_if_needed(_diagnostics_blob);
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
            continue;
        }

        const auto dependency_hash = FILE_WATCHER.generate_file_hash(dependency_path);
        if (!dependency_hash)
        {
            return {};
        }
        dependence += dependency_path;
        dependence += split_char;
        hashes.push_back(*dependency_hash);
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

        const auto current_hash = FILE_WATCHER.generate_file_hash(std::filesystem::path(line.begin(), line.end()));
        if (!current_hash)
        {
            return false;
        }

        hashes.emplace_back(*current_hash);
    }

    if (hashes.empty())
    {
        return false;
    }

    std::ranges::sort(hashes);
    return (hashes | std::views::join_with(split_char) | std::ranges::to<std::string>()) == _fingerprint;
}

}  // namespace

shader_compiler::shader_compiler()
{
    options = {
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::EmitSpirvDirectly,
            {slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
        },
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::VulkanUseEntryPointName,
            {slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
        },
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::Optimization,
            {slang::CompilerOptionValueKind::Int, SlangOptimizationLevel::SLANG_OPTIMIZATION_LEVEL_MAXIMAL, 0, nullptr, nullptr},
        },
        slang::CompilerOptionEntry{
            slang::CompilerOptionName::MatrixLayoutColumn,
            {slang::CompilerOptionValueKind::Int, true, 0, nullptr, nullptr},
        },
    };

    const auto result = slang::createGlobalSession(global_session.writeRef());
    if (!SLANG_SUCCEEDED(result))
    {
        throw std::runtime_error("Cannot create global session.");
    }

    target_desc.format  = SLANG_SPIRV;
    target_desc.profile = global_session->findProfile("spirv_1_4");

    session_desc.targets                  = &target_desc;
    session_desc.targetCount              = 1;
    session_desc.compilerOptionEntries    = options.data();
    session_desc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());


    // load cache
    std::ifstream cache_file(shader_cache_path.data(), std::ios::binary);
    if (!cache_file.is_open())
    {
        return;
    }

    std::string header;
    header.resize(shader_cache_header.size());
    cache_file.read(header.data(), shader_cache_header.size());
    if (header != shader_cache_header)
    {
        return;
    }

    size_t cache_count = 0;
    cache_file.read(reinterpret_cast<char*>(&cache_count), sizeof(cache_count));
    for (uint32_t i = 0; i < cache_count; ++i)
    {
        size_t name_len = 0;
        cache_file.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));
        std::string name(name_len, '\0');
        cache_file.read(name.data(), name_len);

        size_t dependence_len = 0;
        cache_file.read(reinterpret_cast<char*>(&dependence_len), sizeof(dependence_len));
        std::string dependence(dependence_len, '\0');
        cache_file.read(dependence.data(), dependence_len);

        size_t fingerprint_len = 0;
        cache_file.read(reinterpret_cast<char*>(&fingerprint_len), sizeof(fingerprint_len));
        std::string fingerprint(fingerprint_len, '\0');
        cache_file.read(fingerprint.data(), fingerprint_len);

        size_t spirv_size = 0;
        cache_file.read(reinterpret_cast<char*>(&spirv_size), sizeof(spirv_size));
        std::vector<char> spirv(spirv_size, '\0');
        cache_file.read(spirv.data(), spirv_size);

        shader_cache.emplace(std::move(name), shader_cache_info{std::move(dependence), std::move(fingerprint), std::move(spirv)});
    }
}

shader_compiler::~shader_compiler()
{
    std::erase_if(shader_cache, [](const auto& item) static { return !std::filesystem::exists(item.first); });

    std::filesystem::create_directories(std::filesystem::path(shader_cache_path).parent_path());
    std::ofstream cache_file(std::filesystem::path(shader_cache_path), std::ios::binary | std::ios::trunc);
    if (!cache_file.is_open())
    {
        return;
    }

    cache_file.write(shader_cache_header.data(), shader_cache_header.length());

    const size_t shader_count = shader_cache.size();
    cache_file.write(reinterpret_cast<const char*>(&shader_count), sizeof(shader_count));

    for (const auto& [name, info] : shader_cache)
    {
        const size_t name_len = name.size();
        cache_file.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
        cache_file.write(name.data(), name_len);

        const size_t dependence_len = info.dependence.size();
        cache_file.write(reinterpret_cast<const char*>(&dependence_len), sizeof(dependence_len));
        cache_file.write(info.dependence.data(), dependence_len);

        const size_t fingerprint_len = info.fingerprint.size();
        cache_file.write(reinterpret_cast<const char*>(&fingerprint_len), sizeof(fingerprint_len));
        cache_file.write(info.fingerprint.data(), fingerprint_len);

        const size_t spirv_size = info.spirv.size();
        cache_file.write(reinterpret_cast<const char*>(&spirv_size), sizeof(spirv_size));
        cache_file.write(info.spirv.data(), info.spirv.size());
    }
}

std::expected<std::vector<char>, std::string> shader_compiler::compile_shader_to_spv(const std::filesystem::path& _shader_path,
                                                                                     const std::vector<std::string_view>& _entry_name) const
{
    const std::string shader_path = _shader_path.generic_string();
    if (const auto iter = shader_cache.find(shader_path); iter != shader_cache.end() && !iter->second.dependence.empty()
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

    if (!slang_module)
    {
        return std::unexpected(get_diagnostics(diagnostics_blob).empty() ? "Failed to load shader module" :
                                                                           get_diagnostics(diagnostics_blob));
    }
    else
    {
        diagnose_if_needed(diagnostics_blob);
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
        shader_cache.insert_or_assign(shader_path, shader_cache_info{std::move(dependence), std::move(fingerprint), spirv});
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

    if (!slang_module)
    {
        return std::unexpected(get_diagnostics(diagnostics_blob).empty() ? "Failed to load shader module" :
                                                                           get_diagnostics(diagnostics_blob));
    }
    else
    {
        diagnose_if_needed(diagnostics_blob);
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
