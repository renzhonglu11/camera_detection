#include "control/gimbal_controller.h"

#include "hardware/PCA9685.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace camera_detection {

GimbalController::GimbalController(const Options &options)
	: options_(options),
	  pan_{"pan",
		   options.pan_tracking,
		   options.pan_channel,
		   options.pan_min_degree,
		   options.pan_max_degree,
		   options.pan_center_degree,
		   options.pan_center_degree},
	  tilt_{"tilt",
			options.tilt_control,
			options.tilt_channel,
			options.tilt_min_degree,
			options.tilt_max_degree,
			options.tilt_center_degree,
			options.tilt_center_degree} {}

GimbalController::~GimbalController() {
	stop();
}

bool GimbalController::start() {
	if (!pan_.enabled && !tilt_.enabled)
		return true;

	if (!validate_axis(pan_) || !validate_axis(tilt_))
		return false;

	if (pan_.enabled && tilt_.enabled && pan_.channel == tilt_.channel) {
		std::cerr << "Gimbal disabled: pan and tilt both use channel "
				  << pan_.channel << '\n';
		return false;
	}

	std::vector<char> i2c_device(options_.pan_i2c_device.begin(),
								 options_.pan_i2c_device.end());
	i2c_device.push_back('\0');

	if (i2c_init(i2c_device.data(), I2C_ADDR) < 0) {
		std::cerr << "Gimbal disabled: failed to open "
				  << options_.pan_i2c_device << '\n';
		return false;
	}

	i2c_writeReg(MODE1, 0x80);
	usleep(10000);
	PCA9685_setPWMFreq(60);
	ready_ = true;

	// PanTracker centers the pan axis itself so its motion log sees the write.
	if (tilt_.enabled) {
		write(GimbalAxis::Tilt, tilt_.center_degree);
		std::cout << "Tilt control ready: channel=" << tilt_.channel
				  << " range=" << tilt_.min_degree << ".." << tilt_.max_degree
				  << " center=" << tilt_.degree << '\n';
	}

	return true;
}

void GimbalController::stop() {
	if (stopped_)
		return;

	stopped_ = true;
	if (!ready_)
		return;

	if (tilt_.enabled && options_.pan_return_center_on_exit &&
		angle(GimbalAxis::Tilt) != tilt_.center_degree) {
		std::cout << "Tilt shutdown: returning " << angle(GimbalAxis::Tilt)
				  << " -> " << tilt_.center_degree << " deg\n";
		move_to(GimbalAxis::Tilt, tilt_.center_degree,
				options_.pan_return_step, options_.pan_return_step_ms);
	}

	ready_ = false;
	i2c_close();
}

int GimbalController::angle(GimbalAxis axis) const {
	std::lock_guard<std::mutex> lock(mutex_);
	return axis_state(axis).degree;
}

int GimbalController::clamped_degree(GimbalAxis axis, int degree) const {
	const AxisState &state = axis_state(axis);
	return std::clamp(degree, state.min_degree, state.max_degree);
}

int GimbalController::write(GimbalAxis axis, int degree) {
	std::lock_guard<std::mutex> lock(mutex_);
	AxisState &state = axis_state(axis);
	if (!ready_ || !state.enabled)
		return -1;

	const int requested_degree =
		std::clamp(degree, state.min_degree, state.max_degree);
	if (setServoDegree(static_cast<uint8_t>(state.channel),
					   static_cast<uint8_t>(requested_degree)) < 0)
		return -1;

	state.degree = requested_degree;
	return requested_degree;
}

void GimbalController::move_to(GimbalAxis axis, int target_degree, int step,
							   int step_ms) {
	target_degree = clamped_degree(axis, target_degree);
	step = std::max(1, step);
	const useconds_t step_us =
		static_cast<useconds_t>(std::max(1, step_ms)) * 1000;

	for (int current = angle(axis); current != target_degree;
		 current = angle(axis)) {
		const int move = std::clamp(target_degree - current, -step, step);
		if (write(axis, current + move) < 0)
			return;
		usleep(step_us);
	}
}

void GimbalController::run_test_sweep(GimbalAxis axis) {
	const AxisState &state = axis_state(axis);
	std::cout << "Gimbal " << state.name
			  << " test sweep: center -> min -> max -> center"
			  << " step=" << options_.pan_test_step
			  << " step_ms=" << options_.pan_test_step_ms
			  << " hold_ms=" << options_.pan_test_hold_ms << '\n';
	const useconds_t hold_us =
		static_cast<useconds_t>(options_.pan_test_hold_ms) * 1000;

	usleep(hold_us);
	move_to(axis, state.min_degree, options_.pan_test_step,
			options_.pan_test_step_ms);
	usleep(hold_us);
	move_to(axis, state.max_degree, options_.pan_test_step,
			options_.pan_test_step_ms);
	usleep(hold_us);
	move_to(axis, state.center_degree, options_.pan_test_step,
			options_.pan_test_step_ms);
	usleep(hold_us);
}

GimbalController::AxisState &GimbalController::axis_state(GimbalAxis axis) {
	return axis == GimbalAxis::Pan ? pan_ : tilt_;
}

const GimbalController::AxisState &
GimbalController::axis_state(GimbalAxis axis) const {
	return axis == GimbalAxis::Pan ? pan_ : tilt_;
}

bool GimbalController::validate_axis(AxisState &axis) {
	if (!axis.enabled)
		return true;

	if (axis.channel < 0 || axis.channel > 15) {
		std::cerr << "Gimbal disabled: " << axis.name
				  << " channel must be 0..15, got " << axis.channel << '\n';
		return false;
	}

	axis.min_degree = std::clamp(axis.min_degree, 0, 180);
	axis.max_degree = std::clamp(axis.max_degree, 0, 180);
	if (axis.min_degree > axis.max_degree)
		std::swap(axis.min_degree, axis.max_degree);
	axis.center_degree =
		std::clamp(axis.center_degree, axis.min_degree, axis.max_degree);
	axis.degree = axis.center_degree;
	return true;
}

} // namespace camera_detection
