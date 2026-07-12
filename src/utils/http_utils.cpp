#include "utils/http_utils.h"

#include "utils/file_utils.h"

#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace camera_detection {

bool send_all(int fd, const void *data, std::size_t size)
{
	const auto *bytes = static_cast<const unsigned char *>(data);
	std::size_t total_sent = 0;
	while (total_sent < size)
	{
		const ssize_t sent =
			::send(fd, bytes + total_sent, size - total_sent, MSG_NOSIGNAL);
		if (sent <= 0)
			return false;

		total_sent += static_cast<std::size_t>(sent);
	}

	return true;
}

void send_response(int client_fd,
				   std::string_view status,
				   std::string_view content_type,
				   std::string_view body,
				   std::string_view extra_headers)
{
	std::ostringstream response;
	response << "HTTP/1.1 " << status << "\r\n"
			 << "Connection: close\r\n"
			 << "Cache-Control: no-store\r\n"
			 << "Content-Type: " << content_type << "\r\n"
			 << "Content-Length: " << body.size() << "\r\n";
	if (!extra_headers.empty())
		response << extra_headers;

	response << "\r\n" << body;

	const std::string raw = response.str();
	send_all(client_fd, raw.data(), raw.size());
}

std::optional<HttpRequest> read_http_request(int client_fd)
{
	constexpr std::size_t max_header_bytes = 64 * 1024;
	std::string buffer;
	buffer.reserve(4096);

	char chunk[4096];
	std::size_t header_end = std::string::npos;
	while (header_end == std::string::npos)
	{
		const ssize_t received = ::recv(client_fd, chunk, sizeof(chunk), 0);
		if (received <= 0)
			return std::nullopt;

		buffer.append(chunk, static_cast<std::size_t>(received));
		if (buffer.size() > max_header_bytes)
			return std::nullopt;

		header_end = buffer.find("\r\n\r\n");
	}

	HttpRequest request;
	const std::size_t header_block_size = header_end + 4;
	std::istringstream header_stream(buffer.substr(0, header_end));
	std::string request_line;
	if (!std::getline(header_stream, request_line))
		return std::nullopt;

	if (!request_line.empty() && request_line.back() == '\r')
		request_line.pop_back();

	std::istringstream request_line_stream(request_line);
	std::string http_version;
	if (!(request_line_stream >> request.method >> request.path >> http_version))
		return std::nullopt;

	std::string header_line;
	while (std::getline(header_stream, header_line))
	{
		if (!header_line.empty() && header_line.back() == '\r')
			header_line.pop_back();

		const std::size_t separator = header_line.find(':');
		if (separator == std::string::npos)
			continue;

		request.headers.emplace(
			to_lower_copy(trim_copy(std::string_view(header_line).substr(0, separator))),
			trim_copy(std::string_view(header_line).substr(separator + 1)));
	}

	std::size_t content_length = 0;
	const auto content_length_it = request.headers.find("content-length");
	if (content_length_it != request.headers.end())
	{
		content_length = static_cast<std::size_t>(
			std::strtoul(content_length_it->second.c_str(), nullptr, 10));
	}

	request.body.assign(buffer.data() + header_block_size,
						buffer.size() - header_block_size);
	while (request.body.size() < content_length)
	{
		const ssize_t received = ::recv(client_fd, chunk, sizeof(chunk), 0);
		if (received <= 0)
			return std::nullopt;

		request.body.append(chunk, static_cast<std::size_t>(received));
	}

	if (request.body.size() > content_length)
		request.body.resize(content_length);

	return request;
}

std::string json_escape(std::string_view value)
{
	std::ostringstream escaped;
	for (const char ch : value)
	{
		switch (ch)
		{
		case '\\':
			escaped << "\\\\";
			break;
		case '"':
			escaped << "\\\"";
			break;
		case '\n':
			escaped << "\\n";
			break;
		case '\r':
			escaped << "\\r";
			break;
		case '\t':
			escaped << "\\t";
			break;
		default:
			escaped << ch;
			break;
		}
	}

	return escaped.str();
}

std::optional<std::string> extract_json_string_field(const std::string &body,
													 std::string_view key)
{
	const std::string token = "\"" + std::string(key) + "\"";
	const std::size_t key_pos = body.find(token);
	if (key_pos == std::string::npos)
		return std::nullopt;

	std::size_t pos = body.find(':', key_pos + token.size());
	if (pos == std::string::npos)
		return std::nullopt;

	++pos;
	while (pos < body.size() &&
		   std::isspace(static_cast<unsigned char>(body[pos])) != 0)
	{
		++pos;
	}

	if (pos >= body.size() || body[pos] != '"')
		return std::nullopt;

	++pos;
	std::string value;
	while (pos < body.size())
	{
		const char ch = body[pos++];
		if (ch == '"')
			return value;

		if (ch != '\\')
		{
			value.push_back(ch);
			continue;
		}

		if (pos >= body.size())
			return std::nullopt;

		const char escaped = body[pos++];
		switch (escaped)
		{
		case '"':
			value.push_back('"');
			break;
		case '\\':
			value.push_back('\\');
			break;
		case '/':
			value.push_back('/');
			break;
		case 'b':
			value.push_back('\b');
			break;
		case 'f':
			value.push_back('\f');
			break;
		case 'n':
			value.push_back('\n');
			break;
		case 'r':
			value.push_back('\r');
			break;
		case 't':
			value.push_back('\t');
			break;
		default:
			return std::nullopt;
		}
	}

	return std::nullopt;
}

std::optional<int> extract_json_int_field(const std::string &body,
										  std::string_view key)
{
	const std::string token = "\"" + std::string(key) + "\"";
	const std::size_t key_pos = body.find(token);
	if (key_pos == std::string::npos)
		return std::nullopt;

	std::size_t pos = body.find(':', key_pos + token.size());
	if (pos == std::string::npos)
		return std::nullopt;

	++pos;
	while (pos < body.size() &&
		   std::isspace(static_cast<unsigned char>(body[pos])) != 0)
	{
		++pos;
	}

	if (pos >= body.size())
		return std::nullopt;

	char *end = nullptr;
	const long value = std::strtol(body.c_str() + pos, &end, 10);
	if (end == body.c_str() + pos)
		return std::nullopt;

	return static_cast<int>(value);
}

} // namespace camera_detection
