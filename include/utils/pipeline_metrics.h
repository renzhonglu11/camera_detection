#pragma once

#include "utils/app_types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace camera_detection {

using PipelineClock = std::chrono::steady_clock;
using PipelineTimePoint = PipelineClock::time_point;

struct FrameMeta {
	std::uint64_t source_frame_id = 0;
	std::optional<std::chrono::nanoseconds> media_pts;
	std::optional<PipelineTimePoint> scheduled_release;
	PipelineTimePoint received_at;
};

enum class PublishStatus {
	NotAttempted,
	Accepted,
	NoSession,
	CloneFailed,
};

struct FrameMetrics {
	FrameMeta meta;
	PipelineTimePoint read_begin;
	PipelineTimePoint read_end;
	PipelineTimePoint detect_begin;
	PipelineTimePoint detect_end;
	PipelineTimePoint result_ready;
	std::optional<PipelineTimePoint> control_begin;
	std::optional<PipelineTimePoint> control_end;
	std::optional<PipelineTimePoint> overlay_begin;
	std::optional<PipelineTimePoint> overlay_end;
	std::optional<PipelineTimePoint> publish_begin;
	std::optional<PipelineTimePoint> publish_end;
	bool detected = false;
	float center_x = 0.0f;
	float center_y = 0.0f;
	float radius = 0.0f;
	float area = 0.0f;
	int frame_width = 0;
	int frame_height = 0;
	PublishStatus publish_status = PublishStatus::NotAttempted;
};

struct StreamPushMetrics {
	std::uint64_t stream_push_id = 0;
	std::uint64_t source_frame_id = 0;
	PipelineTimePoint received_at;
	PipelineTimePoint pushed_at;
	bool repeated_source_frame = false;
	int flow_return = 0;
};

class PipelineMetrics {
  public:
	PipelineMetrics() = default;
	~PipelineMetrics();

	PipelineMetrics(const PipelineMetrics &) = delete;
	PipelineMetrics &operator=(const PipelineMetrics &) = delete;

	bool start(const Options &options, double source_fps,
			   std::uint64_t expected_frame_count,
			   const std::vector<std::string> &command_line);
	void record_frame(FrameMetrics metrics);
	void record_stream_push(StreamPushMetrics metrics);
	void record_latest_frame_overwrite();
	void stop(std::uint64_t source_frames_read, bool source_failed);

	bool enabled() const { return enabled_; }
	const std::string &error_message() const { return error_message_; }
	const std::string &output_dir() const { return output_dir_; }

  private:
	void writer_loop();
	void write_frame(const FrameMetrics &metrics);
	void write_stream_push(const StreamPushMetrics &metrics);
	void sample_resources();
	void write_run_config(const Options &options, double source_fps,
					  std::uint64_t expected_frame_count,
					  const std::vector<std::string> &command_line);
	void write_runtime_summary();
	std::int64_t elapsed_us(PipelineTimePoint time) const;
	std::string optional_elapsed_us(
		const std::optional<PipelineTimePoint> &time) const;

	bool enabled_ = false;
	std::string error_message_;
	std::string output_dir_;
	std::size_t queue_capacity_ = 0;
	PipelineTimePoint start_time_{};
	std::ofstream frames_file_;
	std::ofstream stream_pushes_file_;
	std::ofstream resources_file_;
	std::thread writer_thread_;
	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::deque<FrameMetrics> frame_queue_;
	std::deque<StreamPushMetrics> stream_queue_;
	bool stop_requested_ = false;
	std::atomic<std::uint64_t> frame_rows_written_{0};
	std::atomic<std::uint64_t> frame_rows_dropped_{0};
	std::atomic<std::uint64_t> stream_rows_written_{0};
	std::atomic<std::uint64_t> stream_rows_dropped_{0};
	std::atomic<std::uint64_t> latest_frame_overwrites_{0};
	std::atomic<std::uint64_t> duplicate_stream_pushes_{0};
	std::atomic<std::uint64_t> failed_stream_pushes_{0};
	std::uint64_t source_frames_read_ = 0;
	bool source_failed_ = false;
};

const char *publish_status_name(PublishStatus status);

} // namespace camera_detection
