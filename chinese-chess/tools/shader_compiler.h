/**
 * @file shader_compiler.h
 * @brief Slang 着色器编译单例：全局 session 与编译选项固定在此，编译结果按「路径 + 入口集合」缓存到磁盘。
 * @details 构造时建一次 IGlobalSession（编译选项、target profile 都在这里定死），每个文件再开一个
 *          轻量 ISession，并把它所在目录作为唯一 search path。缓存键是着色器路径加全部入口名，
 *          命中条件还包括依赖文件的 SHA-256 指纹一致，因此改任何 include 都会自动重编。
 *          缓存文件头里带编译选项指纹与 Slang 构建串，换选项或换 Slang 版本都会整份失效。
 */

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

/**
 * @brief 进程内唯一的 Slang 编译器：两条编译入口 + 一份磁盘缓存。
 * @details 文件版走 loadModule（模块名 = 文件名 stem，search path = 该文件所在目录），编译成功后把
 *          依赖路径、依赖指纹与 SPIR-V 一起写进缓存；字符串版走 loadModuleFromSourceString，模块名
 *          固定 "shaders"、虚拟路径 "memory:shader"，不参与缓存。缓存容器是 mutable，所以两个
 *          编译入口都能保持 const。
 */
class shader_compiler
{
public:
    /**
     * @brief 编译磁盘上的着色器文件，命中缓存时完全不经过 Slang。
     * @param _shader_path .slang 文件路径，同时是缓存键的前半段。
     * @param _entry_name 入口点名字列表，拼成缓存键的后半段，并按顺序合成程序。
     * @return SPIR-V 字节；session 创建、模块加载、入口缺失、链接或取码失败都返回错误串。
     * @note 命中条件：缓存键存在 + 依赖串非空 + 按缓存的依赖路径重算 SHA-256 与 fingerprint 完全一致。
     * @note 依赖无法指纹化（文件不存在 / 读不了）时不写缓存；save_cache 还会剔除路径已不存在的条目。
     */
    [[nodiscard]] std::expected<std::vector<char>, std::string> compile_shader_to_spv(const std::filesystem::path& _shader_path,
                                                                                      const std::vector<std::string_view>& _entry_name) const;
    /**
     * @brief 直接从内存源码加载模块并编译，不查询或更新本类的编译结果缓存。
     * @param _shader_string 完整源码（OCIO 替换后的 vulkan_blend_image.slang）。
     * @param _entry_name 入口点名字列表，按顺序合成程序。
     * @return SPIR-V 字节；模块加载、入口缺失、链接或取码失败都返回错误串。
     * @note 模块名固定 "shaders"、虚拟路径 "memory:shader"；没有额外设置该源码的文件搜索目录，
     *       import 的解析仍取决于 Slang 会话搜索规则，不能据此认为编译过程绝不读盘。
     */
    [[nodiscard]] std::expected<std::vector<char>, std::string> compile_shader_to_spv(const std::string& _shader_string,
                                                                                      const std::vector<std::string_view>& _entry_name) const;
    /**
     * @brief 取全局唯一实例。
     * @return 静态实例的引用；Slang 初始化与缓存读取发生在该静态对象构造时，而非本函数首次调用时。
     */
    [[nodiscard]] static shader_compiler& get_shader_compiler() noexcept;

private:
    /**
     * @brief 建全局 session、填编译选项与 target / session 描述、算缓存头并读缓存。
     * @throws std::runtime_error slang::createGlobalSession 失败。
     */
    shader_compiler();
    /**
     * @brief 析构时把着色器缓存写回磁盘。
     */
    ~shader_compiler();
    /**
     * @brief 禁止拷贝构造：全局 session 与缓存都必须唯一。
     */
    shader_compiler(const shader_compiler&)            = delete;
    /**
     * @brief 禁止拷贝赋值：全局 session 与缓存都必须唯一。
     */
    shader_compiler& operator=(const shader_compiler&) = delete;
    /**
     * @brief 禁止移动构造：实例是静态对象，地址必须稳定。
     */
    shader_compiler(shader_compiler&&)                 = delete;
    /**
     * @brief 禁止移动赋值：实例是静态对象，地址必须稳定。
     */
    shader_compiler& operator=(shader_compiler&&)      = delete;

    /**
     * @brief 读 resources/cache/shader_cache.cache，文件头与当前构建不一致就整份丢弃。
     */
    void load_cache();
    /**
     * @brief 把缓存写回 resources/cache/shader_cache.cache，先剔除源文件已不存在的条目。
     */
    void save_cache() const;

    /** @brief 进程内唯一实例，静态存储期；构造即完成 Slang 初始化与缓存读入。 */
    static shader_compiler               compiler;
    /** @brief Slang 全局会话，只建一次；每个文件 / 每段源码的 session 都由它派生。 */
    Slang::ComPtr<slang::IGlobalSession> global_session;

    /** @brief 编译选项表：直接出 SPIR-V、入口点改名、优化与调试等级、列主序、RayQuery 能力、C 布局。 */
    std::vector<slang::CompilerOptionEntry> options;
    /** @brief 目标描述：SPIR-V 目标与 spirv_1_6 profile。 */
    slang::TargetDesc                       target_desc;
    /** @brief session 描述，指向上面两个成员；文件版会拷贝它再改 search path。 */
    slang::SessionDesc                      session_desc;

    /** @brief 当前构建的缓存文件头，读入时逐字节比对，不一致就整份丢弃。 */
    std::string shader_cache_header;

    /**
     * @brief 一条着色器缓存记录：依赖路径列表、依赖哈希指纹与编译产物。
     * @details dependence 每行一个依赖路径（含 .slang 自己），fingerprint 是这些文件 SHA-256 排序后
     *          用 '\n' 拼起来的串；两者一起决定命中与否。
     */
    struct shader_cache_info
    {
        /** @brief 依赖文件的路径列表，每行一个，顺序与收集时一致。 */
        std::string       dependence;
        /** @brief 依赖哈希指纹，排序后拼接，用于快速判断依赖是否变过。 */
        std::string       fingerprint;
        /** @brief 该缓存键对应的 SPIR-V 字节。 */
        std::vector<char> spirv;
    };
    /** @brief 缓存键（路径 + 入口集合）到缓存记录的映射；mutable 让 const 编译入口也能填缓存。 */
    mutable std::unordered_map<std::string, shader_cache_info> shader_cache;
};
