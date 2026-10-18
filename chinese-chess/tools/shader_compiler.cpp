/**
 * @file shader_compiler.cpp
 * @brief shader_compiler 的实现：Slang session 配置、模块到 SPIR-V 的编译链，以及着色器缓存的读写。
 * @details 缓存是单一二进制文件：文件头（魔数 + Slang 构建串 + Debug/Release + target format/profile +
 *          逐个编译选项的 name/kind/int 与字符串）与每条记录都用「size_t 长度前缀 + 字节」序列化。
 *          读入先比头，不一致就整份丢弃；每段长度直接按 size_t 读入并 resize，截断或异常长度都靠
 *          load_cache 外层的 catch 兜底。依赖指纹是「依赖路径列表 + 排序后的 SHA-256 列表」，
 *          任一依赖缺失或读不了就不写缓存。
 */

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

/** @brief 着色器缓存文件路径，相对运行目录 chinese-chess/。 */
constexpr std::string_view shader_cache_path  = "resources\\cache\\shader_cache.cache";
/** @brief 缓存文件头的魔数，后面跟 '\n' 与构建串等字段。 */
constexpr std::string_view shader_cache_magic = "shader_cache_magic";
/** @brief 缓存键与依赖列表的行分隔符。 */
constexpr char             split_char         = '\n';

/**
 * @brief 取文件字节数，并把读位置复位到开头。
 * @param _file 已打开的输入流。
 * @return 文件大小（streamoff）；流状态异常时 tellg 返回 -1，原样传出。
 */
[[nodiscard]] inline std::streamoff get_file_size(std::ifstream& _file)
{
    _file.seekg(0, std::ios::end);
    const std::streamoff file_size = static_cast<std::streamoff>(_file.tellg());
    _file.seekg(0, std::ios::beg);
    return file_size;
}

/**
 * @brief 读一段「size_t 长度前缀 + 字节」到 blob。
 * @tparam cache_blob 只接受 std::string 或 std::vector<char>。
 * @param _file 输入流。
 * @param _blob 读到的内容；先 resize 再整段读。
 * @return 长度字段读失败返回 false；长度为 0 时返回 true 且不改 _blob；内容读不满也返回 false。
 * @note 长度字段不做上限或剩余字节校验，异常大的值由 load_cache 的 catch 兜底。
 */
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

/**
 * @brief 写一段「size_t 长度前缀 + 字节」，空 span 直接跳过。
 * @param _file 输出流。
 * @param _blob 要写的字节。
 * @note 空 blob 不写长度字段；当前只用来写一定非空的段（头、键、依赖、指纹、SPIR-V），读写两端因此对齐。
 */
inline void write_cache_blob(std::ofstream& _file, const std::span<const char> _blob)
{
    if (!_blob.empty())
    {
        const size_t length = _blob.size();
        _file.write(reinterpret_cast<const char*>(&length), sizeof(length));
        _file.write(_blob.data(), static_cast<std::streamsize>(_blob.size()));
    }
}

/**
 * @brief 生成缓存文件头：魔数、Slang 构建串、构建类型、target format/profile 与全部编译选项。
 * @param _options 编译选项表，逐项取 name / kind / intValue 与两个字符串值。
 * @param _build_tag Slang 的 getBuildTagString() 结果。
 * @param _target_desc 目标描述，取 format 与 profile 的底层值。
 * @return 以 '\n' 分隔的头字符串，读入侧与当前构建的头逐字节比对。
 * @note Debug 与 Release 的构建类型串不同，所以两种配置不共用缓存。
 */
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

/** @brief Debug 下打印入口哈希时用的递增调用序号。 */
constinit int global_counter = 0;

/**
 * @brief 把 Slang 的诊断 blob 转成字符串。
 * @param _diagnostic_blob 诊断 blob，可能为空。
 * @return 诊断文本；blob 为空时返回空串。
 */
[[nodiscard]] std::string get_diagnostics(const Slang::ComPtr<slang::IBlob>& _diagnostic_blob) noexcept
{
    if (_diagnostic_blob == nullptr)
    {
        return {};
    }
    return std::string(static_cast<const char*>(_diagnostic_blob->getBufferPointer()), _diagnostic_blob->getBufferSize());
}

#ifndef NDEBUG
/**
 * @brief Debug 下把非空诊断打印到 stdout。
 * @param _diagnostic_blob Slang 写出的诊断，空则什么都不打印。
 */
void diagnose_if_needed(const Slang::ComPtr<slang::IBlob>& _diagnostic_blob) noexcept
{
    if (_diagnostic_blob != nullptr)
    {
        std::println("{}", get_diagnostics(_diagnostic_blob));
    }
}

/**
 * @brief Debug 下逐个打印入口点在每个 target 上的 Slang 哈希。
 * @param _entrypoint_count 入口点数量。
 * @param _target_count target 数量（当前恒为 1）。
 * @param _composed_program 已合成的程序，哈希由它现算。
 * @note 这些哈希只用于核对缓存是否真的复用了同一次编译，不参与缓存键。
 */
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
/** @brief Release 下的空实现：既没有诊断可打，也不需要入口哈希。 */
void diagnose_if_needed(const Slang::ComPtr<slang::IBlob>&) noexcept {}
/** @brief Release 下的空实现：不打印入口哈希。 */
void print_entrypoint_hashes(int, int, const Slang::ComPtr<slang::IComponentType>&) {}
#endif  // !NDEBUG

/**
 * @brief 把模块按入口点列表合成、链接，再取 target 0 的 SPIR-V。
 * @param _session 当前会话，用于合成组件类型。
 * @param _diagnostics_blob 复用的诊断 blob，每次失败都把它转成错误串。
 * @param _slang_module 已加载的模块。
 * @param _entry_name 入口点名字列表。
 * @return SPIR-V blob；入口缺失、合成、链接或取码失败都返回错误串。
 * @note Debug 下每步都会打印诊断并统计入口哈希，Release 下这两件事都是空操作。
 */
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

/**
 * @brief 收集模块的依赖文件路径，并算出排序后的哈希指纹。
 * @param _module 已加载的 Slang 模块。
 * @return {依赖路径串, 指纹串}；任一依赖不存在或读不了时返回两个空串，表示「这条缓存无法指纹化」。
 * @note 依赖路径由 Slang 给出（含 .slang 自己），原样写进缓存，下次按同一路径重算。
 */
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

/**
 * @brief 按缓存的依赖路径重算哈希，判断是否与指纹一致。
 * @param _dependence 每行一个依赖路径。
 * @param _fingerprint 排序后以 '\n' 拼起来的哈希串。
 * @return 一致返回 true；任一文件读不了或哈希集合为空返回 false。
 */
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

/**
 * @brief 拼缓存键：着色器路径 + '\n' + 各入口名（同样以 '\n' 分隔）。
 * @param _shader_path 着色器路径。
 * @param _entry_names 入口点列表。
 * @return 缓存键；入口集合不同的两条管线不会互相串味。
 */
[[nodiscard]] std::string generate_cache_key(std::string_view _shader_path, const std::vector<std::string_view>& _entry_names)
{
    return std::string(_shader_path) + split_char
           + (_entry_names | std::views::join_with(split_char) | std::ranges::to<std::string>());
}

/**
 * @brief 从缓存键里取回着色器路径部分。
 * @param _cache_key generate_cache_key 生成的键。
 * @return 第一个 '\n' 之前的子串，即路径。
 */
[[nodiscard]] std::string_view get_path_from_cache_key(std::string_view _cache_key) noexcept
{
    return _cache_key.substr(0, _cache_key.find(split_char));
}

}  // namespace

/**
 * @brief 建全局 session、装配编译选项与 target / session 描述，算缓存头后读缓存。
 * @note ForceCLayout 与 VulkanUseEntryPointName 直接决定 GPU 侧布局与入口名，不能随意删。
 * @note 优化与调试等级按构建类型二选一，Debug 是 NONE + MAXIMAL，Release 是 MAXIMAL + NONE。
 */
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

/**
 * @brief 析构时写回缓存；save_cache 内部自己处理目录创建与打开失败。
 */
shader_compiler::~shader_compiler()
{
    save_cache();
}

/**
 * @brief 先查缓存，未命中再现建 session、加载模块并编译，成功后连同依赖指纹写回缓存。
 * @note search path 只放着色器所在目录，同目录的 import 走裸文件名。
 * @note 依赖串为空（无法指纹化）时只编译不缓存，下次启动仍会重新编译。
 */
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

/**
 * @brief 用内存源码建模块并编译，完全不碰缓存与文件系统。
 * @note 会话直接用成员的 session_desc，所以没有额外 search path。
 */
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

/**
 * @brief 打开缓存文件、比对文件头，再逐条读回缓存记录。
 * @note 头不一致、条目数读不满或任一段读失败都直接返回，已读到的条目留在表里。
 * @note 整个读入包在 try/catch 里：异常长度导致的 bad_alloc / length_error 不会让静态构造崩启动。
 */
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

/**
 * @brief 先剔除源文件已不存在的条目，再创建目录并整份重写缓存。
 * @note 输出流打不开就放弃，退出路径不抛异常。
 * @note 文件头也带长度前缀，读侧先读长度再读头的字节。
 */
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
