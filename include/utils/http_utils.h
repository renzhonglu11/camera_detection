#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <map>

namespace camera_detection {

struct HttpRequest
{
	std::string method;
	std::string path;
	std::map<std::string, std::string> headers;
	std::string body;
};

bool send_all(int fd, const void *data, std::size_t size);
void send_response(int client_fd,
				   std::string_view status,
				   std::string_view content_type,
				   std::string_view body,
				   std::string_view extra_headers = {});
std::optional<HttpRequest> read_http_request(int client_fd);
std::string json_escape(std::string_view value);
std::optional<std::string> extract_json_string_field(const std::string &body,
													 std::string_view key);
std::optional<int> extract_json_int_field(const std::string &body,
										  std::string_view key);

} // namespace camera_detection
