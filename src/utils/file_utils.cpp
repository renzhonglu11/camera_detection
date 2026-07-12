#include "utils/file_utils.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace camera_detection {

std::string to_lower_copy(std::string value)
{
	for (char &ch : value)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

	return value;
}

std::string trim_copy(std::string_view value)
{
	std::size_t begin = 0;
	while (begin < value.size() &&
		   std::isspace(static_cast<unsigned char>(value[begin])) != 0)
	{
		++begin;
	}

	std::size_t end = value.size();
	while (end > begin &&
		   std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
	{
		--end;
	}

	return std::string(value.substr(begin, end - begin));
}

std::filesystem::path executable_directory()
{
	std::error_code error;
	const auto executable_path =
		std::filesystem::read_symlink("/proc/self/exe", error);
	if (!error && !executable_path.empty())
		return executable_path.parent_path();

	const auto current_path = std::filesystem::current_path(error);
	if (!error)
		return current_path;

	return ".";
}

std::optional<std::string> read_text_file(const std::filesystem::path &path)
{
	std::ifstream input(path);
	if (!input)
		return std::nullopt;

	std::ostringstream buffer;
	buffer << input.rdbuf();
	return buffer.str();
}

void replace_all(std::string &text,
				 std::string_view needle,
				 std::string_view replacement)
{
	std::size_t position = 0;
	while ((position = text.find(needle, position)) != std::string::npos)
	{
		text.replace(position, needle.size(), replacement);
		position += replacement.size();
	}
}

} // namespace camera_detection
