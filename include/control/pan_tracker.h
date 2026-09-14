#pragma once

#include "detection/ball_detector.h"
#include "utils/app_types.h"

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace camera_detection {

class PanTracker {
public:
	explicit PanTracker(const Options &options);
	~PanTracker();

	bool start();
	void stop();
	void update(const DetectionResult &detection, int frame_width,
			std::uint64_t source_frame_id);

	bool enabled() const { return enabled_; }
	bool ready() const { return ready_; }
	int angle() const { return current_degree_; }
	const std::string &status() const { return status_; }

private:
	int clamped_degree(int degree) const;
	bool can_write(std::chrono::steady_clock::time_point now) const;
	void open_motion_log();
	void stop_motion_log();
	void motion_log_loop();
	void log_motion(std::chrono::steady_clock::time_point write_begin,
					std::chrono::steady_clock::time_point write_end,
					std::optional<std::uint64_t> source_frame_id,
					std::optional<float> pixel_error, int previous_degree,
					int requested_degree, const char *write_result);
	void run_test_sweep();
	void move_to(int target_degree, int step, int step_ms,
				 const char *motion_status);
	void write_degree(int degree, std::chrono::steady_clock::time_point now,
				  std::optional<std::uint64_t> source_frame_id = std::nullopt,
				  std::optional<float> pixel_error = std::nullopt);

	Options options_;
	bool enabled_ = false;
	bool ready_ = false;
	bool stopped_ = false;
	bool in_test_sweep_ = false;
	int current_degree_ = 90;
	std::string status_ = "off";
	std::ofstream motion_log_;
	std::thread motion_log_thread_;
	std::mutex motion_log_mutex_;
	std::condition_variable motion_log_cv_;
	std::deque<std::string> motion_log_queue_;
	bool motion_log_stop_ = false;
	std::chrono::steady_clock::time_point start_time_{};
	std::chrono::steady_clock::time_point last_write_{};
};

} // namespace camera_detection
