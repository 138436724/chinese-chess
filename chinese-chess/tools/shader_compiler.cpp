#include "shader_compiler.h"

#include "file_watcher.h"

#include <fstream>
#include <print>
#include <ranges>
#include <slang-com-helper.h>
#include <slang-com-ptr.h>

shader_compiler shader_compiler::compiler;

std::string shader_compiler::get_diagnostics(const Slang::ComPtr<slang::IBlob>& _diagnostic_blob) noexcept
{
    if (_diagnostic_blob == nullptr)
    {
        return {};
    }
    return std::string(static_cast<const char*>(_diagnostic_blob->getBufferPointer()), _diagnostic_blob->getBufferSize());
}

#ifndef NDEBUG
void shader_compiler::diagnose_if_needed(const Slang::ComPtr<slang::IBlob>& _diagnostic_blob) noexcept
{
    if (_diagnostic_blob != nullptr)
    {
        std::println("{}", get_diagnostics(_diagnostic_blob));
    }
}

void shader_compiler::print_entrypoint_hashes(int                                         _entrypoint_count,
                                              int                                         _target_count,
                                              const Slang::ComPtr<slang::IComponentType>& _composed_program) const
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
void shader_compiler::diagnose_if_needed(const Slang::ComPtr<slang::IBlob>&) noexcept {}
void shader_compiler::print_entrypoint_hashes(int, int, const Slang::ComPtr<slang::IComponentType>&) const {}
#endif  // !NDEBUG

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
}

std::expected<std::vector<char>, std::string> shader_compiler::compile_shader_to_spv(const std::filesystem::path& _shader_path,
                                                                                     const std::vector<std::string_view>& _entry_name) const
{
    std::filesystem::path spirv_path = _shader_path;
    spirv_path.replace_extension(".spv");

    if ((!std::filesystem::exists(_shader_path) || !FILE_WATCHER.is_file_modified(_shader_path)) && std::filesystem::exists(spirv_path))
    {
        std::ifstream in_file(spirv_path.generic_string(), std::ios::ate | std::ios::binary);

        if (in_file.is_open())
        {
            std::vector<char> buffer(in_file.tellg());
            in_file.seekg(0, std::ios::beg);
            in_file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            in_file.close();

            return buffer;
        }
    }

    auto              desc         = session_desc;
    const std::string parent_path  = _shader_path.parent_path().generic_string();
    const std::array  search_paths = {parent_path.c_str()};
    desc.searchPathCount           = 1;
    desc.searchPaths               = search_paths.data();

    auto spirv_code = slang_to_slang_module(desc, _shader_path.stem().generic_string(), true, _entry_name);
    if (!spirv_code)
    {
        return std::unexpected(std::format("Failed to compile shader {}: {}", _shader_path.string(), spirv_code.error()));
    }

    std::ofstream out_file(spirv_path.generic_string(), std::ios::out | std::ios::binary);
    out_file.write(reinterpret_cast<const char*>((*spirv_code)->getBufferPointer()), (*spirv_code)->getBufferSize());
    out_file.close();

    const auto* ptr      = static_cast<const char*>((*spirv_code)->getBufferPointer());
    const auto  byte_num = (*spirv_code)->getBufferSize();
    return std::vector<char>(ptr, ptr + byte_num);
}

std::expected<std::vector<char>, std::string> shader_compiler::compile_shader_to_spv(const std::string& _shader_string,
                                                                                     const std::vector<std::string_view>& _entry_name) const
{
    auto spirv_code = slang_to_slang_module(session_desc, _shader_string, false, _entry_name);
    if (!spirv_code)
    {
        return std::unexpected(std::format("Failed to compile shader string: {}", spirv_code.error()));
    }

    const auto* ptr      = static_cast<const char*>((*spirv_code)->getBufferPointer());
    const auto  byte_num = (*spirv_code)->getBufferSize();
    return std::vector<char>(ptr, ptr + byte_num);
}

std::expected<Slang::ComPtr<slang::IBlob>, std::string> shader_compiler::slang_to_slang_module(const slang::SessionDesc& _session_desc,
                                                                                               const std::string& _shader_string,
                                                                                               bool _as_shader_name,
                                                                                               const std::vector<std::string_view>& _entry_name) const
{
    Slang::ComPtr<slang::ISession> session;
    const auto                     result = global_session->createSession(_session_desc, session.writeRef());
    if (!SLANG_SUCCEEDED(result))
    {
        return std::unexpected("Failed to create Slang session");
    }

    Slang::ComPtr<slang::IBlob>   diagnostics_blob;
    Slang::ComPtr<slang::IModule> slang_module;

    if (_as_shader_name)
    {
        slang_module = session->loadModule(_shader_string.c_str(), diagnostics_blob.writeRef());
    }
    else
    {
        slang_module = session->loadModuleFromSourceString("shaders", "memory:shader", _shader_string.c_str(),
                                                           diagnostics_blob.writeRef());
    }

    diagnose_if_needed(diagnostics_blob);

    if (!slang_module)
    {
        return std::unexpected(get_diagnostics(diagnostics_blob).empty() ? "Failed to load shader module" :
                                                                           get_diagnostics(diagnostics_blob));
    }

    return slang_module_to_spv(session, diagnostics_blob, slang_module, _entry_name);
}

std::expected<Slang::ComPtr<slang::IBlob>, std::string> shader_compiler::slang_module_to_spv(
    Slang::ComPtr<slang::ISession>&      _session,
    Slang::ComPtr<slang::IBlob>&         _diagnostics_blob,
    Slang::ComPtr<slang::IModule>&       _slang_module,
    const std::vector<std::string_view>& _entry_name) const
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

shader_compiler& shader_compiler::get_shader_compiler() noexcept
{
    return compiler;
}
