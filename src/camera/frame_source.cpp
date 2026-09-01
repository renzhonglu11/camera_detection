#include "camera/frame_source.h"

#include "camera/camera_pipeline.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

namespace camera_detection {

FrameSource::FrameSource(const Options &options) : options_(options) {}

bool FrameSource::open() {
	end_of_stream_ = false;
	failed_ = false;
	error_message_.clear();
	next_frame_id_ = 0;
	expected_frame_count_ = 0;
	last_media_pts_.reset();
	replay_start_pts_.reset();

	if (is_file_source()) {
		std::ifstream input(options_.input_path, std::ios::binary);
		if (!input.good()) {
			error_message_ = "Input file does not exist or is not readable: " +
							 options_.input_path;
			failed_ = true;
			return false;
		}

		if (!capture_.open(options_.input_path, cv::CAP_ANY)) {
			error_message_ = "Failed to open input video: " + options_.input_path;
			failed_ = true;
			return false;
		}

		const double frame_count = capture_.get(cv::CAP_PROP_FRAME_COUNT);
		if (std::isfinite(frame_count) && frame_count > 0.0)
			expected_frame_count_ =
				static_cast<std::uint64_t>(std::llround(frame_count));
	} else {
		const std::string pipeline = build_capture_pipeline(options_);
		if (!capture_.open(pipeline, cv::CAP_GSTREAMER)) {
			error_message_ = "Failed to open Raspberry Pi camera pipeline";
			failed_ = true;
			return false;
		}
	}

	source_fps_ = capture_.get(cv::CAP_PROP_FPS);
	if (!std::isfinite(source_fps_) || source_fps_ <= 0.0)
		source_fps_ = static_cast<double>(options_.fps);

	return true;
}

bool FrameSource::read(FramePacket &packet) {
	if (!capture_.isOpened() || end_of_stream_ || failed_)
		return false;

	cv::Mat frame;
	if (!capture_.read(frame)) {
		if (is_file_source() &&
			(expected_frame_count_ == 0 ||
			 next_frame_id_ >= expected_frame_count_)) {
			end_of_stream_ = true;
		} else {
			std::ostringstream message;
			message << "Frame read failed after " << next_frame_id_
					<< " successful frames";
			if (expected_frame_count_ > 0)
				message << "; expected " << expected_frame_count_;
			error_message_ = message.str();
			failed_ = true;
		}
		return false;
	}

	if (frame.empty()) {
		error_message_ = "Frame source returned an empty frame";
		failed_ = true;
		return false;
	}

	packet.frame_id = next_frame_id_++;
	packet.media_pts = current_media_pts();
	if (is_file_source() && options_.replay_mode == ReplayMode::Realtime &&
		packet.media_pts) {
		pace_realtime(*packet.media_pts);
	}
	packet.received_at = std::chrono::steady_clock::now();
	packet.frame = std::move(frame);
	return true;
}

bool FrameSource::is_file_source() const { return !options_.input_path.empty(); }

bool FrameSource::end_of_stream() const { return end_of_stream_; }

bool FrameSource::failed() const { return failed_; }

std::uint64_t FrameSource::expected_frame_count() const {
	return expected_frame_count_;
}

double FrameSource::source_fps() const { return source_fps_; }

const std::string &FrameSource::error_message() const { return error_message_; }

std::optional<std::chrono::nanoseconds> FrameSource::current_media_pts() {
	if (!is_file_source())
		return std::nullopt;

	const double position_ms = capture_.get(cv::CAP_PROP_POS_MSEC);
	std::chrono::nanoseconds pts;
	if (std::isfinite(position_ms) && position_ms >= 0.0) {
		pts = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double, std::milli>(position_ms));
	} else {
		pts = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double>(
				static_cast<double>(next_frame_id_ - 1) / source_fps_));
	}

	if (last_media_pts_ && pts <= *last_media_pts_) {
		pts = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double>(
				static_cast<double>(next_frame_id_ - 1) / source_fps_));
	}

	last_media_pts_ = pts;
	return pts;
}

void FrameSource::pace_realtime(std::chrono::nanoseconds media_pts) {
	if (!replay_start_pts_) {
		replay_start_pts_ = media_pts;
		replay_start_time_ = std::chrono::steady_clock::now();
		return;
	}

	const auto target_time = replay_start_time_ + (media_pts - *replay_start_pts_);
	std::this_thread::sleep_until(target_time);
}

} // namespace camera_detection
