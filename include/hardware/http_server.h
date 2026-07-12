#pragma once

#include <optional>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

#include "utils/app_types.h"
#include "utils/http_utils.h"

namespace camera_detection {

class WebRtcStreamer;

class HttpServer
{
  public:
	HttpServer(const Options &options, WebRtcStreamer *webrtc_streamer);
	~HttpServer();

	bool start();
	void stop();

  private:
	void accept_loop();
	void handle_client(int client_fd);
	void serve_index(int client_fd);
	void serve_healthz(int client_fd);
	void serve_not_found(int client_fd);
	void serve_webrtc_offer(int client_fd);
	void serve_webrtc_answer(int client_fd, const HttpRequest &request);
	void serve_webrtc_candidate(int client_fd, const HttpRequest &request);
	void serve_webrtc_candidates(int client_fd);

	std::optional<std::string> build_index_html() const;

	Options options_;
	WebRtcStreamer *webrtc_streamer_;
	std::atomic<bool> running_{false};
	int server_fd_ = -1;
	std::thread accept_thread_;
	std::mutex client_threads_mutex_;
	std::vector<std::thread> client_threads_;
};

} // namespace camera_detection
