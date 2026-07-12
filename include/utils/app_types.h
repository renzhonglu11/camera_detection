#pragma once

#include <string>

namespace camera_detection {

enum class WebRtcEncoder { Auto, H264Hardware, OpenH264 };

struct Options {
	int width = 512;
	int height = 384;
	int fps = 30;
	int preview_width = 320;
	int display_every = 2;
	int http_port = 8080;
	bool show_preview = false;
	WebRtcEncoder webrtc_encoder = WebRtcEncoder::Auto;
	bool pan_tracking = false;
	std::string pan_i2c_device = "/dev/i2c-1";
	int pan_channel = 1;
	int pan_center_degree = 90;
	int pan_min_degree = 0;
	int pan_max_degree = 180;
	int pan_deadzone = 30;
	int pan_step = 1;
	int pan_interval_ms = 60;
	bool pan_invert = false;
	bool pan_test_sweep = false;
	int pan_test_hold_ms = 1500;
	int pan_test_step = 1;
	int pan_test_step_ms = 30;
	std::string pan_log_csv = "pan_motion.csv";
};

} // namespace camera_detection
