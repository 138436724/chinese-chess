export module record_loader;

import std;

export constexpr std::string_view RECORDS_PATH = "resources\\records\\";

export class record_loader
{
public:
	const std::vector<std::array<wchar_t, 4>> load_record(const std::filesystem::path& _record_path);
	static record_loader& get_record_loader() noexcept;

private:
	record_loader() = default;
	~record_loader() = default;

	record_loader(const record_loader&) = delete;
	record_loader& operator=(const record_loader&) = delete;
	record_loader(const record_loader&&) = delete;
	record_loader& operator=(const record_loader&&) = delete;

	static record_loader loader;
};