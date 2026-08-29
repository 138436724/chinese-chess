#include "file_watcher.h"

#include <array>
#include <format>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <ranges>

namespace {
constexpr std::string_view hash_cache_path   = "resources\\cache\\hash_cache.cache";
constexpr std::string_view hash_cache_header = "hash_cache_header";
}  // namespace

file_watcher file_watcher::watcher;

file_watcher::file_watcher()
{
    std::ifstream cache_file(hash_cache_path.data(), std::ios::binary);
    if (!cache_file.is_open())
    {
        return;
    }

    std::string header;
    header.resize(hash_cache_header.size());
    cache_file.read(header.data(), hash_cache_header.size());
    if (header != hash_cache_header)
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
        cache_file.read(path.data(), path_len);

        size_t hash_len = 0;
        cache_file.read(reinterpret_cast<char*>(&hash_len), sizeof(hash_len));
        std::string hash(hash_len, '\0');
        cache_file.read(hash.data(), hash_len);

        file_watch_cache.emplace(std::move(path), std::move(hash));
    }
}

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
    unsigned int                               hash_len;
    EVP_DigestFinal_ex(sha256.get(), hash.data(), &hash_len);

    return hash | std::views::take(hash_len)
           | std::views::transform([](const auto& _c) static { return std::format("{:02X}", _c); }) | std::views::join
           | std::ranges::to<std::string>();
}

bool file_watcher::is_file_modified(const std::filesystem::path& _file_path) const
{
    const auto        file_hash = generate_file_hash(_file_path);
    const std::string file_path = _file_path.generic_string();

    if (file_hash)
    {
        if (auto iter = file_watch_cache.find(file_path); iter != file_watch_cache.end() && iter->second == *file_hash) [[likely]]
        {
            return false;
        }

        file_watch_cache.insert_or_assign(file_path, *file_hash);
    }

    return true;
}

file_watcher& file_watcher::get_file_watcher() noexcept
{
    return watcher;
}
