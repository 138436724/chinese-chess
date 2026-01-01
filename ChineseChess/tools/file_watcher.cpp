module;

#include <openssl/evp.h>

module file_watcher;

import std.compat;

constexpr const char hash_file_name[] = "resources\\file_watch_cache.hash";
constexpr const char file_cache_header[] = "file_cache_header";

file_watcher file_watcher::watcher;

file_watcher::file_watcher()
{
	if (std::filesystem::exists(hash_file_name))
	{
		std::ifstream file_cache(hash_file_name, std::ios::binary);

		char header[sizeof(file_cache_header)];
		file_cache.read(header, sizeof(file_cache_header));

		if (strcmp(header, file_cache_header) == 0)
		{
			size_t cache_count;
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

				file_watch_cache.insert(std::make_pair(std::string(path.begin(), path.end()), std::string(hash.begin(), hash.end())));
			}
		}

		file_cache.close();
	}
}

file_watcher::~file_watcher()
{
	std::erase_if(file_watch_cache, [](const auto& item) { return !std::filesystem::exists(item.first); });

	std::ofstream file_cache(hash_file_name, std::ios::binary);

	file_cache.write(file_cache_header, sizeof(file_cache_header));

	auto cache_count = file_watch_cache.size();
	file_cache.write(reinterpret_cast<char*>(&cache_count), sizeof(cache_count));

	for (const auto& [path, hash] : file_watch_cache)
	{
		size_t path_len = path.size();
		file_cache.write(reinterpret_cast<char*>(&path_len), sizeof(path_len));
		file_cache.write(path.data(), path_len);
		size_t hash_len = hash.size();
		file_cache.write(reinterpret_cast<char*>(&hash_len), sizeof(hash_len));
		file_cache.write(hash.data(), hash_len);
	}

	file_cache.close();
}

const std::string file_watcher::generate_file_hash(const std::string& _file_path)
{
	std::ifstream file(_file_path.c_str(), std::ios::binary);

	if (!file.is_open())
	{
		return "";
	}

	EVP_MD_CTX* sha256 = EVP_MD_CTX_new();
	EVP_DigestInit_ex(sha256, EVP_sha256(), nullptr);

	char buffer[4096];
	while (true)
	{
		file.read(buffer, 4096);
		EVP_DigestUpdate(sha256, buffer, file.gcount());
		if (file.eof())
		{
			break;
		}
	}

	unsigned char hash[EVP_MAX_MD_SIZE];
	unsigned int hash_len;
	EVP_DigestFinal_ex(sha256, hash, &hash_len);
	EVP_MD_CTX_free(sha256);
	file.close();

	std::stringstream file_hash;
	file_hash << std::hex << std::uppercase << std::setfill('0');
	for (unsigned int i = 0; i < hash_len; ++i)
	{
		file_hash << std::setw(2) << static_cast<int>(hash[i]);
	}
	return file_hash.str();
}

bool file_watcher::is_file_modified(const std::string& _file_path)
{
	bool is_modified = true;

	auto file_hash = generate_file_hash(_file_path);

	if (file_watch_cache.find(_file_path) != file_watch_cache.end())
	{
		is_modified = (file_watch_cache.at(_file_path) != file_hash);
	}

	if (is_modified)
	{
		file_watch_cache.insert_or_assign(_file_path, file_hash);
	}

	return is_modified;
}

file_watcher& file_watcher::get_file_watcher() noexcept
{
	return watcher;
}