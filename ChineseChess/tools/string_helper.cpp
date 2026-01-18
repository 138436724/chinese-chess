module;

#include <unicode/ucsdet.h>
#include <unicode/ucnv.h>
#include <unicode/utypes.h>
#include <unicode/unistr.h>
#include <unicode/ustring.h>
#include <unicode/uchar.h>

#ifdef __INTELLISENSE__
#include <chrono>
#include <fstream>
#endif // __INTELLISENSE__


module string_helper;

void string_helper::trim(std::string& s)
{
	auto start = std::find_if(s.begin(), s.end(), [](const auto& _c) {return !std::iswspace(_c); });
	auto end = std::find_if(s.rbegin(), s.rend(), [](const auto& _c) {return !std::iswspace(_c); }).base();
	s.erase(s.begin(), start);
	s.erase(end, s.end());
}

void string_helper::trim(std::wstring& ws)
{
	auto start = std::find_if(ws.begin(), ws.end(), [](const auto& _c) {return !std::iswspace(_c); });
	auto end = std::find_if(ws.rbegin(), ws.rend(), [](const auto& _c) {return !std::iswspace(_c); }).base();
	ws.erase(ws.begin(), start);
	ws.erase(end, ws.end());
}

std::string string_helper::get_file_encoding(const std::filesystem::path& _file_path)
{
	std::ifstream in_file(_file_path.generic_string(), std::ios::ate | std::ios::binary);
	if (!in_file.is_open())
	{
		throw std::runtime_error("Can not open file!");
	}

	std::vector<char> buffer(in_file.tellg());
	in_file.seekg(0, std::ios::beg);
	in_file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
	in_file.close();

	UErrorCode status = U_ZERO_ERROR;
	UCharsetDetector* detector = ucsdet_open(&status);
	ucsdet_setText(detector, buffer.data(), static_cast<int32_t>(buffer.size()), &status);

	const UCharsetMatch* match = ucsdet_detect(detector, &status);
	if (match == nullptr || U_FAILURE(status))
	{
		ucsdet_close(detector);
		throw std::runtime_error("Detector Failed!");
	}

	std::string encoding = std::string(ucsdet_getName(match, &status));

	ucsdet_close(detector);

	return encoding;
}
