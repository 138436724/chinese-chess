#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#define FILE_WATCHER file_watcher::get_file_watcher()

class file_watcher
{
public:
    [[nodiscard]] bool                 is_file_modified(const std::filesystem::path& _file_path);
    [[nodiscard]] static file_watcher& get_file_watcher() noexcept;

private:
    file_watcher();
    ~file_watcher();
    file_watcher(const file_watcher&)            = delete;
    file_watcher& operator=(const file_watcher&) = delete;
    file_watcher(file_watcher&&)                 = delete;
    file_watcher& operator=(file_watcher&&)      = delete;

    static file_watcher                          watcher;
    std::unordered_map<std::string, std::string> file_watch_cache;
};
