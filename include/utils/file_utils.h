#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace camera_detection {

std::string to_lower_copy(std::string value);
std::string trim_copy(std::string_view value);
std::filesystem::path executable_directory();
std::optional<std::string> read_text_file(const std::filesystem::path &path);
void replace_all(std::string &text,
				 std::string_view needle,
				 std::string_view replacement);

} // namespace camera_detection
