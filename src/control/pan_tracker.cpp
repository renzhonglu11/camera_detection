#include "control/pan_tracker.h"

#include "hardware/PCA9685.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>

namespace camera_detection {

PanTracker::PanTracker(const Options &options)
	: options_(options), enabled_(options.pan_tracking),
	  current_degree_(options.pan_center_degree) {}

PanTracker::~PanTracker() {
	stop();
}

bool PanTracker::start() {
	if (!enabled_) {
		status_ = "off";
		return true;
	}

	options_.pan_min_degree = std::clamp(options_.pan_min_degree, 0, 180);
	options_.pan_max_degree = std::clamp(options_.pan_max_degree, 0, 180);
	if (options_.pan_min_degree > options_.pan_max_degree)
		std::swap(options_.pan_min_degree, options_.pan_max_degree);

	if (options_.pan_channel < 0 || options_.pan_channel > 15) {
		status_ = "bad channel";
		std::cerr << "Pan tracking disabled: --pan-channel must be 0..15, got "
				  << options_.pan_channel << '\n';
		enabled_ = false;
		return false;
	}

	current_degree_ = clamped_degree(options_.pan_center_degree);
	start_time_ = std::chrono::steady_clock::now();

	std::vector<char> i2c_device(options_.pan_i2c_device.begin(),
								 options_.pan_i2c_device.end());
	i2c_device.push_back('\0');

	if (i2c_init(i2c_device.data(), I2C_ADDR) < 0) {
		status_ = "i2c failed";
		std::cerr << "Pan tracking disabled: failed to open "
				  << options_.pan_i2c_device << '\n';
		enabled_ = false;
		return false;
	}

	i2c_writeReg(MODE1, 0x80);
	usleep(10000);
	PCA9685_setPWMFreq(60);
	open_motion_log();
	status_ = "init";
	write_degree(current_degree_, std::chrono::steady_clock::now());
	ready_ = true;
	if (options_.pan_startup_hold_ms > 0) {
		std::cout << "Pan startup: holding center for "
				  << options_.pan_startup_hold_ms << " ms\n";
		usleep(static_cast<useconds_t>(options_.pan_startup_hold_ms) * 1000);
	}
	status_ = "ready";
	std::cout << "Pan tracking ready: device=" << options_.pan_i2c_device
			  << " address=0x" << std::hex << I2C_ADDR << std::dec
			  << " channel=" << options_.pan_channel
			  << " range=" << options_.pan_min_degree << ".."
			  << options_.pan_max_degree << " center=" << current_degree_
			  << " deadzone=" << options_.pan_deadzone
			  << " step=" << options_.pan_step
			  << " interval_ms=" << options_.pan_interval_ms << '\n';

	if (options_.pan_test_sweep)
		run_test_sweep();

	return true;
}

void PanTracker::stop() {
	if (stopped_)
		return;

	stopped_ = true;
	if (enabled_ && ready_ && options_.pan_return_center_on_exit) {
		const int center = clamped_degree(options_.pan_center_degree);
		if (current_degree_ != center) {
			std::cout << "Pan shutdown: returning " << current_degree_ << " -> "
					  << center << " deg"
					  << " step=" << options_.pan_return_step
					  << " step_ms=" << options_.pan_return_step_ms << '\n';
			move_to(center, options_.pan_return_step,
					options_.pan_return_step_ms, "return");
		}
		status_ = "centered";
	}

	ready_ = false;
	stop_motion_log();
}

void PanTracker::update(const DetectionResult &detection, int frame_width,
					std::uint64_t source_frame_id) {
	if (!enabled_ || !ready_)
		return;

	if (!detection.detected || frame_width <= 0) {
		status_ = "hold";
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (!can_write(now))
		return;

	const float frame_center = static_cast<float>(frame_width) * 0.5f;
	const float error = detection.center.x - frame_center;
	if (std::abs(error) <= static_cast<float>(options_.pan_deadzone)) {
		status_ = "centered";
		return;
	}

	const int direction = error > 0.0f ? 1 : -1;
	const int mounted_direction = options_.pan_invert ? -direction : direction;
	const int next_degree =
		clamped_degree(current_degree_ + mounted_direction * options_.pan_step);

	if (next_degree == current_degree_) {
		status_ = "limit";
		last_write_ = now;
		return;
	}

	status_ = mounted_direction > 0 ? "right" : "left";
	write_degree(next_degree, now, source_frame_id, error);
}

int PanTracker::clamped_degree(int degree) const {
	return std::clamp(degree, options_.pan_min_degree, options_.pan_max_degree);
}

bool PanTracker::can_write(std::chrono::steady_clock::time_point now) const {
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		now - last_write_);
	return elapsed.count() >= options_.pan_interval_ms;
}

void PanTracker::open_motion_log() {
	if (options_.pan_log_csv.empty())
		return;

	const std::filesystem::path log_path =
		std::filesystem::absolute(options_.pan_log_csv);
	motion_log_.open(log_path, std::ios::out | std::ios::trunc);
	if (!motion_log_) {
		std::cerr << "Pan tracking: failed to open motion CSV log: "
				  << log_path << '\n';
		return;
	}

	motion_log_ << "elapsed_ms,unix_ms,source_frame_id,channel,pixel_error,"
					"previous_degree,requested_degree,status,write_begin_us,"
					"write_end_us,write_duration_us,write_result,test_sweep\n";
	motion_log_thread_ = std::thread(&PanTracker::motion_log_loop, this);
	std::cout << "Pan motion CSV: " << log_path << '\n';
}

void PanTracker::stop_motion_log() {
	{
		std::lock_guard<std::mutex> lock(motion_log_mutex_);
		motion_log_stop_ = true;
	}
	motion_log_cv_.notify_one();

	if (motion_log_thread_.joinable())
		motion_log_thread_.join();

	if (motion_log_)
		motion_log_.close();
}

void PanTracker::motion_log_loop() {
	for (;;) {
		std::deque<std::string> rows;
		{
			std::unique_lock<std::mutex> lock(motion_log_mutex_);
			motion_log_cv_.wait(lock, [this] {
				return motion_log_stop_ || !motion_log_queue_.empty();
			});

			if (motion_log_queue_.empty() && motion_log_stop_)
				break;

			rows.swap(motion_log_queue_);
		}

		for (const std::string &row : rows)
			motion_log_ << row;
		motion_log_.flush();
	}
}

void PanTracker::log_motion(
	std::chrono::steady_clock::time_point write_begin,
	std::chrono::steady_clock::time_point write_end,
	std::optional<std::uint64_t> source_frame_id,
	std::optional<float> pixel_error, int previous_degree, int requested_degree,
	const char *write_result) {
	if (!motion_log_)
		return;

	const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		write_end - start_time_);
	const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch());
	const auto write_begin_us =
		std::chrono::duration_cast<std::chrono::microseconds>(write_begin -
														 start_time_);
	const auto write_end_us = std::chrono::duration_cast<std::chrono::microseconds>(
		write_end - start_time_);
	const auto write_duration_us =
		std::chrono::duration_cast<std::chrono::microseconds>(write_end -
													 write_begin);

	std::ostringstream row;
	row << elapsed_ms.count() << ',' << unix_ms.count() << ',';
	if (source_frame_id)
		row << *source_frame_id;
	row << ',' << options_.pan_channel << ',';
	if (pixel_error)
		row << *pixel_error;
	row << ',' << previous_degree << ',' << requested_degree << ',' << status_
		<< ',' << write_begin_us.count() << ',' << write_end_us.count() << ','
		<< write_duration_us.count() << ',' << write_result << ','
		<< (in_test_sweep_ ? 1 : 0) << '\n';

	{
		std::lock_guard<std::mutex> lock(motion_log_mutex_);
		motion_log_queue_.push_back(row.str());
	}
	motion_log_cv_.notify_one();
}

void PanTracker::run_test_sweep() {
	std::cout << "Pan test sweep: center -> min -> max -> center"
			  << " step=" << options_.pan_test_step
			  << " step_ms=" << options_.pan_test_step_ms
			  << " hold_ms=" << options_.pan_test_hold_ms << '\n';
	const useconds_t hold_us =
		static_cast<useconds_t>(options_.pan_test_hold_ms) * 1000;

	in_test_sweep_ = true;
	usleep(hold_us);
	move_to(options_.pan_min_degree, options_.pan_test_step,
			options_.pan_test_step_ms, "test");
	usleep(hold_us);
	move_to(options_.pan_max_degree, options_.pan_test_step,
			options_.pan_test_step_ms, "test");
	usleep(hold_us);
	move_to(clamped_degree(options_.pan_center_degree), options_.pan_test_step,
			options_.pan_test_step_ms, "test");
	usleep(hold_us);
	in_test_sweep_ = false;
	status_ = "ready";
}

void PanTracker::move_to(int target_degree, int step, int step_ms,
					 const char *motion_status) {
	target_degree = clamped_degree(target_degree);
	step = std::max(1, step);
	const useconds_t step_us =
		static_cast<useconds_t>(std::max(1, step_ms)) * 1000;

	while (current_degree_ != target_degree) {
		const int delta = target_degree - current_degree_;
		const int move = std::clamp(delta, -step, step);
		status_ = motion_status;
		write_degree(current_degree_ + move, std::chrono::steady_clock::now());
		usleep(step_us);
	}
}

void PanTracker::write_degree(int degree,
							  std::chrono::steady_clock::time_point now,
							  std::optional<std::uint64_t> source_frame_id,
							  std::optional<float> pixel_error) {
	const int previous_degree = current_degree_;
	const int requested_degree = clamped_degree(degree);
	current_degree_ = requested_degree;
	const auto write_begin = std::chrono::steady_clock::now();
	setServoDegree(static_cast<uint8_t>(options_.pan_channel),
				   static_cast<uint8_t>(current_degree_));
	const auto write_end = std::chrono::steady_clock::now();
	last_write_ = now;
	// The legacy PCA9685 API returns void. T03 will propagate hardware errors;
	// until then, do not report an unobservable write as successful.
	log_motion(write_begin, write_end, source_frame_id, pixel_error,
			   previous_degree, requested_degree, "unknown");
}

} // namespace camera_detection
