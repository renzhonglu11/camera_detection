#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include <opencv2/core/mat.hpp>

namespace camera_detection {

struct FramePacket {
	std::uint64_t frame_id = 0;
	std::optional<std::chrono::nanoseconds> media_pts;
	std::optional<std::chrono::steady_clock::time_point> scheduled_release;
	std::chrono::steady_clock::time_point received_at;
	cv::Mat frame;
};

} // namespace camera_detection
