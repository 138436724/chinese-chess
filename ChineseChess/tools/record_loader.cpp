module record_loader;

record_loader record_loader::loader;

const std::vector<std::array<wchar_t, 4>> record_loader::load_record(const std::filesystem::path& _record_path)
{
	std::ifstream f(_record_path.generic_string());
	if (!f.is_open())
	{
		throw std::runtime_error("Can not open file!");
	}

	std::vector<std::array<wchar_t, 4>> all_records;

	std::string line;
	while (std::getline(f, line))
	{
		std::wstring w_field = std::filesystem::path(line).generic_wstring();
		std::wstringstream w_ss(w_field);

		while (std::getline(w_ss, w_field, L'，'))
		{
			auto start = std::find_if(w_field.begin(), w_field.end(), [](const auto& _c) {return !std::iswspace(_c); });
			auto end = std::find_if(w_field.rbegin(), w_field.rend(), [](const auto& _c) {return !std::iswspace(_c); }).base();
			w_field.erase(w_field.begin(), start);
			w_field.erase(end, w_field.end());

			std::array<wchar_t, 4> w_record;
			std::copy(w_field.begin(), w_field.end(), w_record.begin());
			all_records.push_back(std::move(w_record));
		}
	}

	f.close();

	return all_records;
}

record_loader& record_loader::get_record_loader() noexcept
{
	return loader;
}
