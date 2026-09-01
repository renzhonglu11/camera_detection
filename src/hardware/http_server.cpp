#include "hardware/http_server.h"

#include "streaming/webrtc_streamer.h"
#include "utils/file_utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>

namespace camera_detection {

HttpServer::HttpServer(const Options &options, WebRtcStreamer *webrtc_streamer)
	: options_(options), webrtc_streamer_(webrtc_streamer) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
	server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd_ < 0) {
		std::perror("socket");
		return false;
	}

	const int reuse = 1;
	::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(static_cast<uint16_t>(options_.http_port));
	if (::bind(server_fd_, reinterpret_cast<sockaddr *>(&address),
			   sizeof(address)) < 0) {
		std::perror("bind");
		return false;
	}

	if (::listen(server_fd_, 8) < 0) {
		std::perror("listen");
		return false;
	}

	running_ = true;
	accept_thread_ = std::thread(&HttpServer::accept_loop, this);
	return true;
}

void HttpServer::stop() {
	if (!running_)
		return;

	running_ = false;
	if (server_fd_ >= 0) {
		::shutdown(server_fd_, SHUT_RDWR);
		::close(server_fd_);
		server_fd_ = -1;
	}

	if (accept_thread_.joinable())
		accept_thread_.join();

	std::lock_guard<std::mutex> lock(client_threads_mutex_);
	for (auto &thread : client_threads_) {
		if (thread.joinable())
			thread.join();
	}
	client_threads_.clear();
}

void HttpServer::accept_loop() {
	while (running_) {
		sockaddr_in client_address{};
		socklen_t client_length = sizeof(client_address);
		const int client_fd =
			::accept(server_fd_, reinterpret_cast<sockaddr *>(&client_address),
					 &client_length);
		if (client_fd < 0) {
			if (running_)
				std::perror("accept");
			break;
		}

		std::lock_guard<std::mutex> lock(client_threads_mutex_);
		client_threads_.emplace_back(&HttpServer::handle_client, this,
									 client_fd);
	}
}

void HttpServer::handle_client(int client_fd) {
	const auto request = read_http_request(client_fd);
	if (!request) {
		::close(client_fd);
		return;
	}

	if (request->method == "GET" && request->path == "/")
		serve_index(client_fd);
	else if (request->method == "GET" && request->path == "/healthz")
		serve_healthz(client_fd);
	else if (request->method == "GET" && request->path == "/api/webrtc/offer")
		serve_webrtc_offer(client_fd);
	else if (request->method == "GET" &&
			 request->path == "/api/webrtc/candidates")
		serve_webrtc_candidates(client_fd);
	else if (request->method == "POST" && request->path == "/api/webrtc/answer")
		serve_webrtc_answer(client_fd, *request);
	else if (request->method == "POST" &&
			 request->path == "/api/webrtc/candidate")
		serve_webrtc_candidate(client_fd, *request);
	else
		serve_not_found(client_fd);

	::close(client_fd);
}

std::optional<std::string> HttpServer::build_index_html() const {
	const auto template_path = executable_directory() / "index.html";
	auto html = read_text_file(template_path);
	if (!html)
		return std::nullopt;

	replace_all(*html, "__BACKEND_NAME__", "webrtc");
	replace_all(*html, "__WIDTH__", std::to_string(options_.width));
	replace_all(*html, "__HEIGHT__", std::to_string(options_.height));
	replace_all(*html, "__CAPTURE_FPS__", std::to_string(options_.fps));
	replace_all(*html, "__STREAM_FPS__", std::to_string(options_.stream_fps));
	return html;
}

void HttpServer::serve_index(int client_fd) {
	const auto html = build_index_html();
	if (!html) {
		send_response(client_fd, "500 Internal Server Error",
					  "text/plain; charset=utf-8",
					  "Missing frontend asset: index.html\n");
		return;
	}

	send_response(client_fd, "200 OK", "text/html; charset=utf-8", *html);
}

void HttpServer::serve_healthz(int client_fd) {
	send_response(client_fd, "200 OK", "application/json; charset=utf-8",
				  "{\"ok\":true}\n");
}

void HttpServer::serve_not_found(int client_fd) {
	send_response(client_fd, "404 Not Found", "text/plain; charset=utf-8",
				  "Not found\n");
}

void HttpServer::serve_webrtc_offer(int client_fd) {
	if (webrtc_streamer_ == nullptr) {
		send_response(client_fd, "404 Not Found", "text/plain; charset=utf-8",
					  "WebRTC backend is disabled\n");
		return;
	}

	std::string offer_sdp;
	std::string error_message;
	int http_status = 500;
	if (!webrtc_streamer_->create_offer(offer_sdp, error_message,
										http_status)) {
		const char *status_text =
			http_status == 409 ? "409 Conflict" : "500 Internal Server Error";
		send_response(client_fd, status_text, "text/plain; charset=utf-8",
					  error_message + '\n');
		return;
	}

	const std::string body =
		"{\"type\":\"offer\",\"sdp\":\"" + json_escape(offer_sdp) + "\"}\n";
	send_response(client_fd, "200 OK", "application/json; charset=utf-8", body);
}

void HttpServer::serve_webrtc_answer(int client_fd,
									 const HttpRequest &request) {
	if (webrtc_streamer_ == nullptr) {
		send_response(client_fd, "404 Not Found", "text/plain; charset=utf-8",
					  "WebRTC backend is disabled\n");
		return;
	}

	const auto type = extract_json_string_field(request.body, "type");
	const auto sdp = extract_json_string_field(request.body, "sdp");
	if (!type || !sdp || *type != "answer") {
		send_response(client_fd, "400 Bad Request", "text/plain; charset=utf-8",
					  "Expected JSON body with "
					  "{\"type\":\"answer\",\"sdp\":\"...\"}\n");
		return;
	}

	std::string error_message;
	int http_status = 500;
	if (!webrtc_streamer_->set_answer(*sdp, error_message, http_status)) {
		const char *status_text =
			http_status == 409 ? "409 Conflict" : "500 Internal Server Error";
		send_response(client_fd, status_text, "text/plain; charset=utf-8",
					  error_message + '\n');
		return;
	}

	send_response(client_fd, "200 OK", "application/json; charset=utf-8",
				  "{\"ok\":true}\n");
}

void HttpServer::serve_webrtc_candidate(int client_fd,
										const HttpRequest &request) {
	if (webrtc_streamer_ == nullptr) {
		send_response(client_fd, "404 Not Found", "text/plain; charset=utf-8",
					  "WebRTC backend is disabled\n");
		return;
	}

	const auto sdp_mline_index =
		extract_json_int_field(request.body, "sdpMLineIndex");
	const auto candidate = extract_json_string_field(request.body, "candidate");
	if (!sdp_mline_index || !candidate) {
		send_response(client_fd, "400 Bad Request", "text/plain; charset=utf-8",
					  "Expected JSON body with "
					  "{\"sdpMLineIndex\":N,\"candidate\":\"...\"}\n");
		return;
	}

	std::string error_message;
	int http_status = 500;
	if (!webrtc_streamer_->add_remote_candidate(*sdp_mline_index, *candidate,
												error_message, http_status)) {
		const char *status_text =
			http_status == 409 ? "409 Conflict" : "500 Internal Server Error";
		send_response(client_fd, status_text, "text/plain; charset=utf-8",
					  error_message + '\n');
		return;
	}

	send_response(client_fd, "200 OK", "application/json; charset=utf-8",
				  "{\"ok\":true}\n");
}

void HttpServer::serve_webrtc_candidates(int client_fd) {
	if (webrtc_streamer_ == nullptr) {
		send_response(client_fd, "404 Not Found", "text/plain; charset=utf-8",
					  "WebRTC backend is disabled\n");
		return;
	}

	send_response(client_fd, "200 OK", "application/json; charset=utf-8",
				  webrtc_streamer_->local_candidates_json());
}

} // namespace camera_detection
