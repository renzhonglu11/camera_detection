#pragma once

#include "utils/app_types.h"
#include "utils/frame_packet.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <opencv2/videoio.hpp>

namespace camera_detection {

class FrameSource {
  public:
	explicit FrameSource(const Options &options);

	bool open();
	bool read(FramePacket &packet);
	bool is_file_source() const;
	bool end_of_stream() const;
	bool failed() const;
	std::uint64_t expected_frame_count() const;
	double source_fps() const;
	const std::string &error_message() const;

  private:
	std::optional<std::chrono::nanoseconds> current_media_pts();
	void pace_realtime(std::chrono::nanoseconds media_pts);

	Options options_;
	cv::VideoCapture capture_;
	std::uint64_t next_frame_id_ = 0;
	std::uint64_t expected_frame_count_ = 0;
	double source_fps_ = 0.0;
	bool end_of_stream_ = false;
	bool failed_ = false;
	std::string error_message_;
	std::optional<std::chrono::nanoseconds> last_media_pts_;
	std::optional<std::chrono::nanoseconds> replay_start_pts_;
	std::chrono::steady_clock::time_point replay_start_time_;
};

} // namespace camera_detection
