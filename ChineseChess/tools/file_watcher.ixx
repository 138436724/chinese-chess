export module file_watcher;

import std;

export class file_watcher
{
public:
	const std::string generate_file_hash(const std::string& _file_path);
	bool is_file_modified(const std::string& _file_path);
	static file_watcher& get_file_watcher() noexcept;

private:
	file_watcher();
	~file_watcher();
	file_watcher(const file_watcher&) = delete;
	file_watcher& operator=(const file_watcher&) = delete;
	file_watcher(const file_watcher&&) = delete;
	file_watcher& operator=(const file_watcher&&) = delete;

	static file_watcher watcher;
	std::unordered_map<std::string, std::string> file_watch_cache;
};