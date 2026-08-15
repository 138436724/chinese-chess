#include "file_watcher.h"

#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <ranges>

namespace {
constexpr std::string_view hash_file_name    = "resources\\file_watch_cache.hash";
constexpr std::string_view file_cache_header = "file_cache_header";

[[nodiscard]] std::expected<std::string, std::string> generate_file_hash(const std::filesystem::path& _file_path)
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
}  // namespace

file_watcher file_watcher::watcher;

file_watcher::file_watcher()
{
    if (std::filesystem::exists(hash_file_name))
    {
        std::ifstream file_cache(hash_file_name.data(), std::ios::binary);

        std::string header;
        header.resize(file_cache_header.size());
        file_cache.read(header.data(), file_cache_header.size());

        if (header == file_cache_header)
        {
            size_t cache_count = 0;
            file_cache.read(reinterpret_cast<char*>(&cache_count), sizeof(cache_count));

            for (size_t i = 0; i < cache_count; i++)
            {
                size_t path_len = 0;
                file_cache.read(reinterpret_cast<char*>(&path_len), sizeof(path_len));
                std::vector<char> path(path_len, '\0');
                file_cache.read(path.data(), path_len);

                size_t hash_len = 0;
                file_cache.read(reinterpret_cast<char*>(&hash_len), sizeof(hash_len));
                std::vector<char> hash(hash_len, '\0');
                file_cache.read(hash.data(), hash_len);

                file_watch_cache.emplace(std::string(path.begin(), path.end()), std::string(hash.begin(), hash.end()));
            }
        }

        file_cache.close();
    }
}

file_watcher::~file_watcher()
{
    std::erase_if(file_watch_cache, [](const auto& item) static { return !std::filesystem::exists(item.first); });

    std::ofstream file_cache(hash_file_name.data(), std::ios::binary);

    file_cache.write(file_cache_header.data(), file_cache_header.length());

    const auto cache_count = file_watch_cache.size();
    file_cache.write(reinterpret_cast<const char*>(&cache_count), sizeof(cache_count));

    for (const auto& [path, hash] : file_watch_cache)
    {
        const size_t path_len = path.size();
        file_cache.write(reinterpret_cast<const char*>(&path_len), sizeof(path_len));
        file_cache.write(path.data(), path_len);
        const size_t hash_len = hash.size();
        file_cache.write(reinterpret_cast<const char*>(&hash_len), sizeof(hash_len));
        file_cache.write(hash.data(), hash_len);
    }

    file_cache.close();
}

bool file_watcher::is_file_modified(const std::filesystem::path& _file_path)
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
