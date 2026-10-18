/**
 * @file file_watcher.cpp
 * @brief file_watcher 的实现：SHA-256 计算与 hash 缓存文件的读写。
 * @details 缓存文件是自定义二进制格式：先写死字符串头，再写 size_t 条目数，最后每条依次是
 *          「size_t 路径长度 + 路径字节 + size_t 哈希长度 + 哈希字节」，全部按本机字节序。
 *          构造读、析构写；写入前先剔除磁盘上已经不存在的路径，避免缓存无限增长。
 *          哈希计算走 OpenSSL 的 EVP 接口，读盘用 4 KiB 栈缓冲，结果按大写十六进制展开。
 */

#include "file_watcher.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace {
/** @brief 哈希缓存文件路径，相对运行目录 chinese-chess/。 */
constexpr std::string_view hash_cache_path   = "resources\\cache\\hash_cache.cache";
/** @brief 缓存文件头，内容与长度都写死；读到的头不匹配就把整份缓存丢弃。 */
constexpr std::string_view hash_cache_header = "hash_cache_header";
}  // namespace

file_watcher file_watcher::watcher;

/**
 * @brief 读入 hash 缓存：文件打不开或头不匹配就直接返回，缓存保持为空。
 * @note 不逐条校验长度或流状态；截断的缓存可能留下部分/残缺条目，析构的路径存在性检查不能验证其哈希。
 */
file_watcher::file_watcher()
{
    std::ifstream cache_file(hash_cache_path.data(), std::ios::binary);
    if (!cache_file.is_open())
    {
        return;
    }

    std::string header;
    header.resize(hash_cache_header.size());
    cache_file.read(header.data(), static_cast<std::streamsize>(hash_cache_header.size()));
    if (!cache_file || header != hash_cache_header)
    {
        return;
    }

    size_t cache_count = 0;
    cache_file.read(reinterpret_cast<char*>(&cache_count), sizeof(cache_count));
    for (size_t i = 0; i < cache_count; i++)
    {
        size_t path_len = 0;
        cache_file.read(reinterpret_cast<char*>(&path_len), sizeof(path_len));
        std::string path(path_len, '\0');
        cache_file.read(path.data(), static_cast<std::streamsize>(path_len));

        size_t hash_len = 0;
        cache_file.read(reinterpret_cast<char*>(&hash_len), sizeof(hash_len));
        std::string hash(hash_len, '\0');
        cache_file.read(hash.data(), static_cast<std::streamsize>(hash_len));

        file_watch_cache.emplace(std::move(path), std::move(hash));
    }
}

/**
 * @brief 先剔除文件已不存在的条目，再创建目录并把整份缓存写回。
 * @note 输出流打不开就返回；filesystem 操作及内存分配异常未捕获，逸出析构会触发 terminate。
 */
file_watcher::~file_watcher()
{
    std::erase_if(file_watch_cache, [](const auto& item) static { return !std::filesystem::exists(item.first); });

    std::filesystem::create_directories(std::filesystem::path(hash_cache_path).parent_path());
    std::ofstream cache_file(hash_cache_path.data(), std::ios::binary);
    if (!cache_file.is_open())
    {
        return;
    }

    cache_file.write(hash_cache_header.data(), hash_cache_header.length());

    const auto cache_count = file_watch_cache.size();
    cache_file.write(reinterpret_cast<const char*>(&cache_count), sizeof(cache_count));

    for (const auto& [path, hash] : file_watch_cache)
    {
        const size_t path_len = path.size();
        cache_file.write(reinterpret_cast<const char*>(&path_len), sizeof(path_len));
        cache_file.write(path.data(), path_len);

        const size_t hash_len = hash.size();
        cache_file.write(reinterpret_cast<const char*>(&hash_len), sizeof(hash_len));
        cache_file.write(hash.data(), hash_len);
    }
}

/**
 * @brief 用 OpenSSL EVP 接口算文件的 SHA-256，并按两位大写十六进制展开。
 * @note 仅文件打开失败与 EVP_MD_CTX_new 返回空指针被转为 expected 错误；分配等异常仍可能传播。
 * @warning 当前未检查 EVP 操作的返回值，也未处理非 EOF 的流读取错误；此处并非完整的错误恢复边界。
 * @note 摘要循环以「读完一块后是否 eof」为终止条件，gcount 决定本次喂入的字节数。
 */
std::expected<std::string, std::string> file_watcher::generate_file_hash(const std::filesystem::path& _file_path)
{
    std::ifstream file(_file_path, std::ios::binary);

    if (!file.is_open())
    {
        return std::unexpected(std::format("Failed to read file {}.", _file_path.generic_string()));
    }

    EVP_MD_CTX* raw_sha256 = EVP_MD_CTX_new();
    if (!raw_sha256)
    {
        return std::unexpected("Failed to new EVP_MD_CTX.");
    }

    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> sha256(raw_sha256, EVP_MD_CTX_free);
    EVP_DigestInit_ex(sha256.get(), EVP_sha256(), nullptr);

    std::array<char, 4096> buffer{};
    while (true)
    {
        file.read(buffer.data(), buffer.size());
        EVP_DigestUpdate(sha256.get(), buffer.data(), file.gcount());
        if (file.eof())
        {
            break;
        }
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int                               hash_len = 0;
    EVP_DigestFinal_ex(sha256.get(), hash.data(), &hash_len);

    return hash | std::views::take(hash_len)
           | std::views::transform([](const auto& _c) static { return std::format("{:02X}", _c); }) | std::views::join
           | std::ranges::to<std::string>();
}

/**
 * @brief 比对哈希缓存：读不出哈希就返回 false，否则命中且相同即未修改，其余情况写回并返回 true。
 * @note 缓存是 mutable 的，所以这个 const 方法会改表；单线程使用无需加锁。
 */
bool file_watcher::is_file_modified(const std::filesystem::path& _file_path) const
{
    // 这是“检查并更新基线”，不是纯查询；同一文件紧接着再查一次会得到未修改。
    const auto file_hash = generate_file_hash(_file_path);
    const auto file_path = _file_path.generic_string();

    if (!file_hash)
    {
        return false;
    }

    if (auto iter = file_watch_cache.find(file_path); iter != file_watch_cache.end() && iter->second == *file_hash) [[likely]]
    {
        return false;
    }

    file_watch_cache.insert_or_assign(file_path, *file_hash);
    return true;
}

file_watcher& file_watcher::get_file_watcher() noexcept
{
    return watcher;
}
