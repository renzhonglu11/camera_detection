#include "camera/camera_pipeline.h"
#include "control/pan_tracker.h"
#include "hardware/http_server.h"
#include "streaming/webrtc_streamer.h"
#include "utils/app_types.h"
#include "utils/signal_utils.h"

#include <gst/gst.h>
#include <opencv2/core/mat.hpp>
#include <opencv2/opencv.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "detection/ball_detector.h"

namespace {

bool parse_int_arg(const std::string &arg, const std::string &prefix,
				   int &value) {
	if (arg.rfind(prefix, 0) != 0)
		return false;

	const auto parsed = arg.substr(prefix.size());
	if (parsed.empty())
		return false;

	char *end = nullptr;
	const long converted = std::strtol(parsed.c_str(), &end, 10);
	if (end == nullptr || *end != '\0' || converted <= 0)
		return false;

	value = static_cast<int>(converted);
	return true;
}

bool parse_nonnegative_int_arg(const std::string &arg,
							   const std::string &prefix, int &value) {
	if (arg.rfind(prefix, 0) != 0)
		return false;

	const auto parsed = arg.substr(prefix.size());
	if (parsed.empty())
		return false;

	char *end = nullptr;
	const long converted = std::strtol(parsed.c_str(), &end, 10);
	if (end == nullptr || *end != '\0' || converted < 0)
		return false;

	value = static_cast<int>(converted);
	return true;
}

bool parse_string_arg(const std::string &arg, const std::string &prefix,
					  std::string &value) {
	if (arg.rfind(prefix, 0) != 0)
		return false;

	value = arg.substr(prefix.size());
	return !value.empty();
}

std::string to_lower_copy(std::string value) {
	for (char &ch : value)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

	return value;
}

std::optional<camera_detection::WebRtcEncoder>
parse_webrtc_encoder(std::string value) {
	using camera_detection::WebRtcEncoder;

	value = to_lower_copy(std::move(value));
	if (value == "auto")
		return WebRtcEncoder::Auto;
	if (value == "h264-hw")
		return WebRtcEncoder::H264Hardware;
	if (value == "openh264")
		return WebRtcEncoder::OpenH264;

	return std::nullopt;
}

const char *webrtc_encoder_name(camera_detection::WebRtcEncoder encoder) {
	using camera_detection::WebRtcEncoder;

	switch (encoder) {
	case WebRtcEncoder::Auto:
		return "auto";
	case WebRtcEncoder::H264Hardware:
		return "h264-hw";
	case WebRtcEncoder::OpenH264:
		return "openh264";
	}

	return "unknown";
}

camera_detection::Options parse_args(int argc, char **argv) {
	using camera_detection::Options;

	Options options;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		std::string value;

		if (arg == "--no-display") {
			options.show_preview = false;
			continue;
		}

		if (arg == "--display") {
			options.show_preview = true;
			continue;
		}

		if (arg == "--pan-tracking") {
			options.pan_tracking = true;
			continue;
		}

		if (arg == "--no-pan-tracking") {
			options.pan_tracking = false;
			continue;
		}

		if (arg == "--pan-invert") {
			options.pan_invert = true;
			continue;
		}

		if (arg == "--pan-test-sweep") {
			options.pan_test_sweep = true;
			continue;
		}

		if (parse_int_arg(arg, "--width=", options.width) ||
			parse_int_arg(arg, "--height=", options.height) ||
			parse_int_arg(arg, "--fps=", options.fps) ||
			parse_int_arg(arg, "--preview-width=", options.preview_width) ||
			parse_int_arg(arg, "--display-every=", options.display_every) ||
			parse_int_arg(arg, "--http-port=", options.http_port) ||
			parse_int_arg(arg, "--pan-step=", options.pan_step) ||
			parse_int_arg(arg, "--pan-interval-ms=", options.pan_interval_ms) ||
			parse_int_arg(arg, "--pan-test-hold-ms=",
						  options.pan_test_hold_ms) ||
			parse_int_arg(arg, "--pan-test-step=", options.pan_test_step) ||
			parse_int_arg(arg, "--pan-test-step-ms=",
						  options.pan_test_step_ms)) {
			continue;
		}

		if (parse_nonnegative_int_arg(arg, "--pan-channel=",
									  options.pan_channel) ||
			parse_nonnegative_int_arg(arg, "--pan-center-degree=",
									  options.pan_center_degree) ||
			parse_nonnegative_int_arg(arg, "--pan-min-degree=",
									  options.pan_min_degree) ||
			parse_nonnegative_int_arg(arg, "--pan-max-degree=",
									  options.pan_max_degree) ||
			parse_nonnegative_int_arg(arg, "--pan-deadzone=",
									  options.pan_deadzone)) {
			continue;
		}

		if (parse_string_arg(arg, "--pan-i2c-device=",
							 options.pan_i2c_device)) {
			continue;
		}

		if (parse_string_arg(arg, "--pan-log-csv=", options.pan_log_csv)) {
			continue;
		}

		if (parse_string_arg(arg, "--webrtc-encoder=", value)) {
			const auto encoder = parse_webrtc_encoder(value);
			if (!encoder) {
				std::cerr << "Invalid --webrtc-encoder value: " << value
						  << '\n';
				std::exit(2);
			}

			options.webrtc_encoder = *encoder;
			continue;
		}

		std::cerr
			<< "Unknown argument: " << arg << '\n'
			<< "Usage: camera_detection [--display] [--no-display] [--width=N] "
			   "[--height=N] [--fps=N] "
			   "[--preview-width=N] [--display-every=N] [--http-port=N] "
			   "[--webrtc-encoder=auto|h264-hw|openh264] "
			   "[--pan-tracking] [--pan-i2c-device=PATH] [--pan-channel=N] "
			   "[--pan-center-degree=N] [--pan-min-degree=N] "
			   "[--pan-max-degree=N] [--pan-deadzone=N] [--pan-step=N] "
			   "[--pan-interval-ms=N] [--pan-invert] [--pan-test-sweep] "
			   "[--pan-test-hold-ms=N] [--pan-test-step=N] "
			   "[--pan-test-step-ms=N] [--pan-log-csv=PATH]\n";
		std::exit(2);
	}

	return options;
}

} // namespace

int main(int argc, char **argv) {
	using namespace camera_detection;

	install_signal_handlers();
	gst_init(nullptr, nullptr);

	const Options options = parse_args(argc, argv);
	const std::string pipeline = build_capture_pipeline(options);
	cv::VideoCapture cap(pipeline, cv::CAP_GSTREAMER);
	if (!cap.isOpened()) {
		std::cerr << "Camera open failed\n";
		return -1;
	}

	auto webrtc_streamer = std::make_unique<WebRtcStreamer>(options);
	HttpServer http_server(options, webrtc_streamer.get());
	if (!http_server.start()) {
		std::cerr << "Failed to start HTTP server on port " << options.http_port
				  << '\n';
		return -1;
	}

	std::cout << "Preview page: http://<pi-ip>:" << options.http_port << "/\n";
	std::cout << "Stream backend: webrtc\n";
	std::cout << "WebRTC encoder preference: "
			  << webrtc_encoder_name(options.webrtc_encoder) << '\n';

	PanTracker pan_tracker(options);
	pan_tracker.start();
	std::cout << "Pan tracking: " << pan_tracker.status() << '\n';

	if (options.show_preview)
		cv::namedWindow("Camera", cv::WINDOW_AUTOSIZE);

	cv::Mat frame;
	cv::Mat preview_frame;
	std::size_t capture_frame_count = 0;
	std::size_t displayed_frame_count = 0;
	std::size_t display_skip_counter = 0;
	double capture_fps = 0.0;
	double display_fps = 0.0;
	auto fps_window_start = std::chrono::steady_clock::now();

	// Main loop: capture frames, publish to WebRTC, and optionally display
	// preview
	while (!stop_requested()) {
		if (!cap.read(frame) || frame.empty())
			break;

		++capture_frame_count;
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				now - fps_window_start);
		if (elapsed.count() >= 1000) {
			capture_fps = capture_frame_count * 1000.0 /
						  static_cast<double>(elapsed.count());
			display_fps = displayed_frame_count * 1000.0 /
						  static_cast<double>(elapsed.count());

			std::cout << "capture_fps=" << capture_fps
					  << " display_fps=" << display_fps
					  << " resolution=" << frame.cols << 'x' << frame.rows
					  << '\n';

			capture_frame_count = 0;
			displayed_frame_count = 0;
			fps_window_start = now;
		}

		DetectionResult det = detect_ball(frame);
		// cv::Mat mask;
		// DetectionResult det = detect_ball(frame, &mask);
		pan_tracker.update(det, frame.cols);

		// cv::Mat overlay = frame.clone();
		// overlay.setTo(cv::Scalar(255, 0, 255), mask); // 把 mask 区域染成紫色
		// cv::addWeighted(overlay, 0.35, frame, 0.65, 0.0, frame);

		cv::putText(frame,
					cv::format("CAP %.1f DISP %.1f", capture_fps, display_fps),
					cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8,
					cv::Scalar(0, 255, 0), 2, cv::LINE_AA);

		if (det.detected) {
			cv::circle(frame, det.center, static_cast<int>(det.radius),
					   cv::Scalar(0, 0, 255), 2);

			cv::circle(frame, det.center, 4, cv::Scalar(255, 0, 0), -1);

			cv::putText(frame,
						cv::format("Blue ball x=%.0f y=%.0f r=%.1f",
								   det.center.x, det.center.y, det.radius),
						cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7,
						cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
		} else {
			cv::putText(frame, "Blue ball: not found", cv::Point(10, 60),
						cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2,
						cv::LINE_AA);
		}

		if (pan_tracker.enabled()) {
			cv::putText(frame,
						cv::format("Pan %d deg %s", pan_tracker.angle(),
								   pan_tracker.status().c_str()),
						cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.7,
						cv::Scalar(255, 255, 0), 2, cv::LINE_AA);
		}

		webrtc_streamer->publish_frame(frame);

		if (options.show_preview) {
			++display_skip_counter;
			if (display_skip_counter >=
				static_cast<std::size_t>(options.display_every)) {
				display_skip_counter = 0;
				if (options.preview_width > 0 &&
					frame.cols > options.preview_width) {
					const int preview_height =
						frame.rows * options.preview_width / frame.cols;
					cv::resize(frame, preview_frame,
							   cv::Size(options.preview_width, preview_height),
							   0.0, 0.0, cv::INTER_LINEAR);
					cv::imshow("Camera", preview_frame);
				} else {
					cv::imshow("Camera", frame);
				}

				++displayed_frame_count;
			}

			if (cv::waitKey(1) == 27)
				break;
		}
	}

	http_server.stop();
	webrtc_streamer->stop();
	return 0;
}
