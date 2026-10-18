/**
 * @file file_watcher.h
 * @brief 文件内容哈希与变更判定：SHA-256 摘要 + 进程内 memo + 跨进程的缓存文件。
 * @details 单例在静态初始化期读出 resources/cache/hash_cache.cache 里的「路径 -> 哈希」表，
 *          析构时把磁盘上仍存在的条目写回去，供下次启动时比较；每次 is_file_modified 仍会重算内容哈希。
 *          is_file_modified 只比内容哈希、不看 mtime：内容没变就不算修改。
 */

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <unordered_map>

#define FILE_WATCHER file_watcher::get_file_watcher()

/**
 * @brief 文件变更检测单例：提供 SHA-256 计算与带缓存的「是否被改过」判定。
 * @details 缓存分两层：进程内的 file_watch_cache（mutable，const 查询也会写）与退出时落盘的
 *          hash_cache.cache。路径键统一用 generic_string，所以缓存里存的是正斜杠形式。
 *          不加锁，调用方须串行访问；纹理重载与 shader 依赖指纹计算都会使用它。
 */
class file_watcher
{
public:
    /**
     * @brief 计算文件的 SHA-256 摘要，并按大写十六进制展开。
     * @param _file_path 目标文件路径，以二进制方式分块读取全部内容。
     * @return 成功返回 64 个字符的大写十六进制串；打开失败或 EVP_MD_CTX 创建失败返回错误说明。
     * @note 4 KiB 分块读，每块喂给 EVP_DigestUpdate；空文件也会走一次 0 字节的更新。
     * @note 摘要只与内容有关，不看时间戳；未检查 EVP 返回码或非 EOF 读取错误，不能保证这些失败可恢复。
     */
    [[nodiscard]] static std::expected<std::string, std::string> generate_file_hash(const std::filesystem::path& _file_path);
    /**
     * @brief 与缓存的哈希比较，判断文件自上次记录以来是否变过。
     * @param _file_path 目标文件路径，内部转成 generic_string 作为缓存键。
     * @return 变了或首次见到返回 true；内容未变或 generate_file_hash 返回 expected 错误时返回 false。
     * @note generate_file_hash 返回错误时不更新缓存，并返回 false；false 不能区分未修改与读取失败。
     * @note 命中缓存且哈希相同走 [[likely]] 早退，否则写回新哈希再返回 true。
     */
    [[nodiscard]] bool                 is_file_modified(const std::filesystem::path& _file_path) const;
    /**
     * @brief 取全局唯一实例。
     * @return 静态实例的引用；构造发生在静态初始化期（读缓存），析构时回写缓存。
     */
    [[nodiscard]] static file_watcher& get_file_watcher() noexcept;

private:
    /**
     * @brief 构造时尝试读入 hash 缓存；文件缺失或头读取失败/不匹配时保留空表。
     * @warning 条目长度和后续读取结果尚未校验；截断文件可能留下部分条目，分配异常会向外传播。
     */
    file_watcher();
    /**
     * @brief 析构时剔除文件已不存在的条目，再把整份缓存写回磁盘。
     */
    ~file_watcher();
    /**
     * @brief 禁止拷贝构造：缓存表与静态实例都必须唯一。
     */
    file_watcher(const file_watcher&)            = delete;
    /**
     * @brief 禁止拷贝赋值：缓存表与静态实例都必须唯一。
     */
    file_watcher& operator=(const file_watcher&) = delete;
    /**
     * @brief 禁止移动构造：实例是静态对象，地址必须稳定。
     */
    file_watcher(file_watcher&&)                 = delete;
    /**
     * @brief 禁止移动赋值：实例是静态对象，地址必须稳定。
     */
    file_watcher& operator=(file_watcher&&)      = delete;

    /** @brief 进程内唯一实例，静态存储期：构造即读缓存，析构即回写缓存。 */
    static file_watcher                                  watcher;
    /** @brief 路径（generic_string）到 SHA-256 十六进制串的 memo；mutable 让 const 查询也能更新。 */
    mutable std::unordered_map<std::string, std::string> file_watch_cache;
};
