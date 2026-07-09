#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#define FILE_WATCHER file_watcher::get_file_watcher()

class file_watcher
{
public:
    std::string          generate_file_hash(const std::filesystem::path& _file_path) noexcept;
    bool                 is_file_modified(const std::filesystem::path& _file_path) noexcept;
    static file_watcher& get_file_watcher() noexcept;

private:
    file_watcher();
    ~file_watcher();
    file_watcher(const file_watcher&)             = delete;
    file_watcher& operator=(const file_watcher&)  = delete;
    file_watcher(const file_watcher&&)            = delete;
    file_watcher& operator=(const file_watcher&&) = delete;

    static file_watcher                          watcher;
    std::unordered_map<std::string, std::string> file_watch_cache;
};
