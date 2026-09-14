#include "camera/frame_source.h"
#include "detection/ball_detector.h"
#include "utils/app_types.h"
#include "utils/pipeline_metrics.h"
#include "utils/signal_utils.h"

#include <opencv2/core/mat.hpp>
#include <opencv2/opencv.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifndef CAMERA_DETECTION_WITH_PI_MODULES
#define CAMERA_DETECTION_WITH_PI_MODULES 1
#endif

#if CAMERA_DETECTION_WITH_PI_MODULES
#include "control/pan_tracker.h"
#include "hardware/http_server.h"
#include "streaming/webrtc_streamer.h"

#include <gst/gst.h>
#endif

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

bool parse_size_arg(const std::string &arg, const std::string &prefix,
					std::size_t &value) {
	if (arg.rfind(prefix, 0) != 0)
		return false;

	const auto parsed = arg.substr(prefix.size());
	if (parsed.empty())
		return false;

	char *end = nullptr;
	const unsigned long long converted = std::strtoull(parsed.c_str(), &end, 10);
	if (end == nullptr || *end != '\0' || converted == 0 ||
		converted > std::numeric_limits<std::size_t>::max()) {
		return false;
	}

	value = static_cast<std::size_t>(converted);
	return true;
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

std::optional<camera_detection::ReplayMode> parse_replay_mode(
	std::string value) {
	using camera_detection::ReplayMode;

	value = to_lower_copy(std::move(value));
	if (value == "unpaced")
		return ReplayMode::Unpaced;
	if (value == "realtime")
		return ReplayMode::Realtime;

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

		if (arg == "--no-stream") {
			options.stream_enabled = false;
			continue;
		}

		if (arg == "--stream") {
			options.stream_enabled = true;
			continue;
		}

		if (arg == "--no-metrics") {
			options.metrics_dir.clear();
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

		if (arg == "--no-pan-invert") {
			options.pan_invert = false;
			continue;
		}

		if (arg == "--pan-return-center-on-exit") {
			options.pan_return_center_on_exit = true;
			continue;
		}

		if (arg == "--no-pan-return-center-on-exit") {
			options.pan_return_center_on_exit = false;
			continue;
		}

		if (arg == "--pan-test-sweep") {
			options.pan_test_sweep = true;
			continue;
		}

		if (parse_int_arg(arg, "--width=", options.width) ||
			parse_int_arg(arg, "--height=", options.height) ||
			parse_int_arg(arg, "--fps=", options.fps) ||
			parse_int_arg(arg, "--stream-fps=", options.stream_fps) ||
			parse_int_arg(arg, "--preview-width=", options.preview_width) ||
			parse_int_arg(arg, "--display-every=", options.display_every) ||
			parse_int_arg(arg, "--http-port=", options.http_port) ||
			parse_int_arg(arg, "--pan-step=", options.pan_step) ||
			parse_int_arg(arg, "--pan-interval-ms=", options.pan_interval_ms) ||
			parse_int_arg(arg, "--pan-return-step=", options.pan_return_step) ||
			parse_int_arg(arg, "--pan-return-step-ms=",
						  options.pan_return_step_ms) ||
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
									  options.pan_deadzone) ||
			parse_nonnegative_int_arg(arg, "--pan-startup-hold-ms=",
									  options.pan_startup_hold_ms)) {
			continue;
		}

		if (parse_string_arg(arg, "--pan-i2c-device=",
							 options.pan_i2c_device)) {
			continue;
		}

		if (parse_string_arg(arg, "--pan-log-csv=", options.pan_log_csv)) {
			continue;
		}

		if (parse_string_arg(arg, "--input=", options.input_path)) {
			continue;
		}

		if (parse_string_arg(arg, "--metrics-dir=", options.metrics_dir)) {
			continue;
		}

		if (parse_size_arg(arg, "--metrics-queue-capacity=",
						   options.metrics_queue_capacity)) {
			continue;
		}

		if (parse_string_arg(arg, "--replay=", value)) {
			const auto replay_mode = parse_replay_mode(value);
			if (!replay_mode) {
				std::cerr << "Invalid --replay value: " << value << '\n';
				std::exit(2);
			}

			options.replay_mode = *replay_mode;
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
			<< "Usage: camera_detection [--display] [--no-display] [--stream] "
			   "[--no-stream] [--input=PATH] "
			   "[--replay=unpaced|realtime] [--width=N] "
			   "[--metrics-dir=PATH] [--metrics-queue-capacity=N] "
			   "[--no-metrics] "
			   "[--height=N] [--fps=N] [--stream-fps=N] "
			   "[--preview-width=N] [--display-every=N] [--http-port=N] "
			   "[--webrtc-encoder=auto|h264-hw|openh264] "
			   "[--pan-tracking] [--pan-i2c-device=PATH] [--pan-channel=N] "
			   "[--pan-center-degree=N] [--pan-min-degree=N] "
			   "[--pan-max-degree=N] [--pan-deadzone=N] [--pan-step=N] "
			   "[--pan-interval-ms=N] [--pan-invert] [--no-pan-invert] "
			   "[--pan-test-sweep] "
			   "[--pan-startup-hold-ms=N] [--pan-return-center-on-exit] "
			   "[--no-pan-return-center-on-exit] [--pan-return-step=N] "
			   "[--pan-return-step-ms=N] "
			   "[--pan-test-hold-ms=N] [--pan-test-step=N] "
			   "[--pan-test-step-ms=N] [--pan-log-csv=PATH]\n";
		std::exit(2);
	}

	if (options.pan_test_sweep)
		options.pan_tracking = true;
	if (!options.input_path.empty() && options.pan_tracking) {
		std::cerr << "Offline input cannot be combined with pan tracking or "
					 "pan test sweep; no servo output is allowed during replay\n";
		std::exit(2);
	}
	if (options.stream_fps > options.fps) {
		std::cerr << "Invalid --stream-fps value: " << options.stream_fps
				  << " exceeds capture --fps=" << options.fps << '\n';
		std::exit(2);
	}

	return options;
}

} // namespace

int main(int argc, char **argv) {
	using namespace camera_detection;

	install_signal_handlers();
	const Options options = parse_args(argc, argv);

#if !CAMERA_DETECTION_WITH_PI_MODULES
	if (options.input_path.empty()) {
		std::cerr << "This build supports file replay only; pass --input=PATH\n";
		return 2;
	}
	if (options.stream_enabled) {
		std::cerr << "This build has no streaming modules; pass --no-stream\n";
		return 2;
	}
#endif

#if CAMERA_DETECTION_WITH_PI_MODULES
	PanTracker pan_tracker(options);
	const bool pan_started = pan_tracker.start();
	std::cout << "Pan tracking: " << pan_tracker.status() << '\n';

	if (options.pan_test_sweep) {
		if (!pan_started || !pan_tracker.ready()) {
			std::cerr << "Pan test sweep failed\n";
			return 1;
		}

		std::cout << "Pan test sweep complete; exiting without ball detection\n";
		pan_tracker.stop();
		return 0;
	}

	gst_init(nullptr, nullptr);
#endif

	FrameSource frame_source(options);
	if (!frame_source.open()) {
		std::cerr << frame_source.error_message() << '\n';
#if CAMERA_DETECTION_WITH_PI_MODULES
		pan_tracker.stop();
#endif
		return 1;
	}

	if (frame_source.is_file_source()) {
		std::cout << "Input mode: offline file\n"
				  << "Input path: " << options.input_path << '\n'
				  << "Replay mode: "
				  << (options.replay_mode == ReplayMode::Realtime ? "realtime"
																			 : "unpaced")
				  << '\n'
				  << "Source FPS: " << frame_source.source_fps()
				  << " expected frames: "
				  << frame_source.expected_frame_count() << '\n';
	} else {
		std::cout << "Input mode: Raspberry Pi camera\n";
	}

	std::vector<std::string> command_line;
	command_line.reserve(static_cast<std::size_t>(argc));
	for (int i = 0; i < argc; ++i)
		command_line.emplace_back(argv[i]);

	PipelineMetrics pipeline_metrics;
	if (!pipeline_metrics.start(options, frame_source.source_fps(),
								frame_source.expected_frame_count(), command_line)) {
		std::cerr << pipeline_metrics.error_message() << '\n';
#if CAMERA_DETECTION_WITH_PI_MODULES
		pan_tracker.stop();
#endif
		return 1;
	}
	if (pipeline_metrics.enabled())
		std::cout << "Pipeline metrics: " << pipeline_metrics.output_dir() << '\n';

#if CAMERA_DETECTION_WITH_PI_MODULES
	std::unique_ptr<WebRtcStreamer> webrtc_streamer;
	std::unique_ptr<HttpServer> http_server;
	if (options.stream_enabled) {
		webrtc_streamer = std::make_unique<WebRtcStreamer>(
			options, pipeline_metrics.enabled() ? &pipeline_metrics : nullptr);
		http_server =
			std::make_unique<HttpServer>(options, webrtc_streamer.get());
		if (!http_server->start()) {
			std::cerr << "Failed to start HTTP server on port "
					  << options.http_port << '\n';
			pan_tracker.stop();
			return 1;
		}

		std::cout << "Preview page: http://<pi-ip>:" << options.http_port
				  << "/\n";
		std::cout << "Stream backend: webrtc\n";
		std::cout << "WebRTC encoder preference: "
				  << webrtc_encoder_name(options.webrtc_encoder) << '\n';
	} else {
		std::cout << "Streaming: disabled\n";
	}
#else
	std::cout << "Streaming: unavailable in minimal replay build\n";
#endif

	std::cout << "Capture FPS target: " << options.fps
			  << " WebRTC FPS target: " << options.stream_fps << '\n';

	if (options.show_preview)
		cv::namedWindow("Camera", cv::WINDOW_AUTOSIZE);

	cv::Mat preview_frame;
	BallDetector ball_detector;
	FramePacket packet;
	std::uint64_t processed_frame_count = 0;
	std::size_t capture_frame_count = 0;
	std::size_t displayed_frame_count = 0;
	std::size_t display_skip_counter = 0;
	double capture_fps = 0.0;
	double display_fps = 0.0;
	auto fps_window_start = std::chrono::steady_clock::now();

	const bool needs_output_frame =
		options.show_preview || options.stream_enabled;

	// Main loop: read frames, detect the ball, and optionally publish/display.
	while (!stop_requested()) {
		const auto read_begin = PipelineClock::now();
		if (!frame_source.read(packet))
			break;
		const auto read_end = PipelineClock::now();

		cv::Mat &frame = packet.frame;
		FrameMetrics frame_metrics;
		frame_metrics.meta.source_frame_id = packet.frame_id;
		frame_metrics.meta.media_pts = packet.media_pts;
		frame_metrics.meta.scheduled_release = packet.scheduled_release;
		frame_metrics.meta.received_at = packet.received_at;
		frame_metrics.read_begin = read_begin;
		frame_metrics.read_end = read_end;
		++processed_frame_count;
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

		frame_metrics.detect_begin = PipelineClock::now();
		DetectionResult det = ball_detector.detect(frame);
		frame_metrics.detect_end = PipelineClock::now();
		frame_metrics.result_ready = frame_metrics.detect_end;
		frame_metrics.detected = det.detected;
		frame_metrics.center_x = det.center.x;
		frame_metrics.center_y = det.center.y;
		frame_metrics.radius = det.radius;
		frame_metrics.area = det.area;
		frame_metrics.frame_width = frame.cols;
		frame_metrics.frame_height = frame.rows;

#if CAMERA_DETECTION_WITH_PI_MODULES
		frame_metrics.control_begin = PipelineClock::now();
		pan_tracker.update(det, frame.cols, packet.frame_id);
		frame_metrics.control_end = PipelineClock::now();
#endif

		if (needs_output_frame) {
			frame_metrics.overlay_begin = PipelineClock::now();
			cv::putText(
				frame, cv::format("CAP %.1f DISP %.1f", capture_fps, display_fps),
				cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8,
				cv::Scalar(0, 255, 0), 2, cv::LINE_AA);

			if (det.detected) {
				cv::circle(frame, det.center, static_cast<int>(det.radius),
						   cv::Scalar(0, 0, 255), 2);
				cv::circle(frame, det.center, 4, cv::Scalar(255, 0, 0), -1);
				cv::putText(
					frame,
					cv::format("Blue ball x=%.0f y=%.0f r=%.1f", det.center.x,
							   det.center.y, det.radius),
					cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.7,
					cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
			} else {
				cv::putText(frame, "Blue ball: not found", cv::Point(10, 60),
							cv::FONT_HERSHEY_SIMPLEX, 0.7,
							cv::Scalar(0, 0, 255), 2, cv::LINE_AA);
			}

#if CAMERA_DETECTION_WITH_PI_MODULES
			if (pan_tracker.enabled()) {
				cv::putText(frame,
							cv::format("Pan %d deg %s", pan_tracker.angle(),
									   pan_tracker.status().c_str()),
							cv::Point(10, 90), cv::FONT_HERSHEY_SIMPLEX, 0.7,
							cv::Scalar(255, 255, 0), 2, cv::LINE_AA);
			}
#endif
			frame_metrics.overlay_end = PipelineClock::now();
		}

#if CAMERA_DETECTION_WITH_PI_MODULES
		if (webrtc_streamer) {
			frame_metrics.publish_begin = PipelineClock::now();
			frame_metrics.publish_status =
				webrtc_streamer->publish_frame(frame, frame_metrics.meta);
			frame_metrics.publish_end = PipelineClock::now();
		}
#endif

		pipeline_metrics.record_frame(std::move(frame_metrics));

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

	int exit_code = 0;
	if (frame_source.failed()) {
		std::cerr << frame_source.error_message() << '\n';
		exit_code = 1;
	} else if (frame_source.end_of_stream()) {
		std::cout << "offline_replay_complete frames=" << processed_frame_count
				  << " expected_frames=" << frame_source.expected_frame_count()
				  << '\n';
	}

#if CAMERA_DETECTION_WITH_PI_MODULES
	pan_tracker.stop();
	if (http_server)
		http_server->stop();
	if (webrtc_streamer)
		webrtc_streamer->stop();
#endif
	pipeline_metrics.stop(processed_frame_count, frame_source.failed());

	return exit_code;
}
