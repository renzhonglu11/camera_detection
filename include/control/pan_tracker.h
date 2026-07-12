#pragma once

#include "detection/ball_detector.h"
#include "utils/app_types.h"

#include <condition_variable>
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace camera_detection {

class PanTracker {
public:
	explicit PanTracker(const Options &options);
	~PanTracker();

	bool start();
	void update(const DetectionResult &detection, int frame_width);

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
	void log_motion(std::chrono::steady_clock::time_point now);
	void run_test_sweep();
	void test_move_to(int target_degree);
	void write_degree(int degree, std::chrono::steady_clock::time_point now);

	Options options_;
	bool enabled_ = false;
	bool ready_ = false;
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
