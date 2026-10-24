#pragma once

#include "utils/app_types.h"

#include <mutex>

namespace camera_detection {

enum class GimbalAxis { Pan, Tilt };

// Owns the PCA9685 I2C connection and every servo write for both axes, so
// pan tracking and manual tilt/pan commands never open the device twice.
class GimbalController {
public:
	explicit GimbalController(const Options &options);
	~GimbalController();

	bool start();
	void stop();

	bool ready() const { return ready_; }
	bool enabled(GimbalAxis axis) const { return axis_state(axis).enabled; }
	int angle(GimbalAxis axis) const;
	int clamped_degree(GimbalAxis axis, int degree) const;

	// Writes a clamped angle and returns it, or -1 when the write failed.
	int write(GimbalAxis axis, int degree);
	void move_to(GimbalAxis axis, int target_degree, int step, int step_ms);
	void run_test_sweep(GimbalAxis axis);

private:
	struct AxisState {
		const char *name;
		bool enabled;
		int channel;
		int min_degree;
		int max_degree;
		int center_degree;
		int degree;
	};

	AxisState &axis_state(GimbalAxis axis);
	const AxisState &axis_state(GimbalAxis axis) const;
	bool validate_axis(AxisState &axis);

	Options options_;
	AxisState pan_;
	AxisState tilt_;
	bool ready_ = false;
	bool stopped_ = false;
	mutable std::mutex mutex_;
};

} // namespace camera_detection
