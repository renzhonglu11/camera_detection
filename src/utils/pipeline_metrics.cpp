#include "utils/pipeline_metrics.h"

#include <sys/resource.h>

#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace camera_detection {

namespace {

std::string json_escape(const std::string &value) {
	std::ostringstream escaped;
	for (const unsigned char ch : value) {
		switch (ch) {
		case '\\':
			escaped << "\\\\";
			break;
		case '"':
			escaped << "\\\"";
			break;
		case '\n':
			escaped << "\\n";
			break;
		case '\r':
			escaped << "\\r";
			break;
		case '\t':
			escaped << "\\t";
			break;
		default:
			if (ch < 0x20) {
				escaped << "\\u" << std::hex << std::setw(4)
						<< std::setfill('0') << static_cast<int>(ch) << std::dec;
			} else {
				escaped << static_cast<char>(ch);
			}
		}
	}
	return escaped.str();
}

const char *replay_mode_name(ReplayMode mode) {
	return mode == ReplayMode::Realtime ? "realtime" : "unpaced";
}

std::string utc_now() {
	const auto now = std::chrono::system_clock::now();
	const std::time_t time = std::chrono::system_clock::to_time_t(now);
	std::tm utc{};
	gmtime_r(&time, &utc);
	std::ostringstream value;
	value << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
	return value.str();
}

struct ResourceSample {
	long rss_kb = -1;
	long peak_rss_kb = -1;
	long threads = -1;
	long fd_count = -1;
	double user_cpu_ms = -1.0;
	double system_cpu_ms = -1.0;
};

ResourceSample read_resources() {
	ResourceSample sample;
	std::ifstream status("/proc/self/status");
	std::string key;
	while (status >> key) {
		if (key == "VmRSS:") {
			status >> sample.rss_kb;
		} else if (key == "VmHWM:") {
			status >> sample.peak_rss_kb;
		} else if (key == "Threads:") {
			status >> sample.threads;
		}
		std::string remainder;
		std::getline(status, remainder);
	}

	std::error_code error;
	long fd_count = 0;
	for (std::filesystem::directory_iterator iterator("/proc/self/fd", error), end;
		 !error && iterator != end; iterator.increment(error)) {
		++fd_count;
	}
	if (!error)
		sample.fd_count = fd_count;

	rusage usage{};
	if (getrusage(RUSAGE_SELF, &usage) == 0) {
		sample.user_cpu_ms = usage.ru_utime.tv_sec * 1000.0 +
			usage.ru_utime.tv_usec / 1000.0;
		sample.system_cpu_ms = usage.ru_stime.tv_sec * 1000.0 +
			usage.ru_stime.tv_usec / 1000.0;
	}

	return sample;
}

} // namespace

const char *publish_status_name(PublishStatus status) {
	switch (status) {
	case PublishStatus::NotAttempted:
		return "not_attempted";
	case PublishStatus::Accepted:
		return "accepted";
	case PublishStatus::NoSession:
		return "no_session";
	case PublishStatus::CloneFailed:
		return "clone_failed";
	}
	return "unknown";
}

PipelineMetrics::~PipelineMetrics() { stop(source_frames_read_, source_failed_); }

bool PipelineMetrics::start(const Options &options, double source_fps,
						std::uint64_t expected_frame_count,
						const std::vector<std::string> &command_line) {
	if (options.metrics_dir.empty())
		return true;

	output_dir_ = options.metrics_dir;
	queue_capacity_ = options.metrics_queue_capacity;
	start_time_ = PipelineClock::now();

	std::error_code error;
	std::filesystem::create_directories(output_dir_, error);
	if (error) {
		error_message_ = "Failed to create metrics directory " + output_dir_ +
			": " + error.message();
		return false;
	}

	frames_file_.open(std::filesystem::path(output_dir_) / "frames.csv",
					  std::ios::out | std::ios::trunc);
	stream_pushes_file_.open(
		std::filesystem::path(output_dir_) / "stream_pushes.csv",
		std::ios::out | std::ios::trunc);
	resources_file_.open(std::filesystem::path(output_dir_) / "resources.csv",
						 std::ios::out | std::ios::trunc);
	if (!frames_file_ || !stream_pushes_file_ || !resources_file_) {
		error_message_ = "Failed to open metrics CSV files in " + output_dir_;
		return false;
	}

	frames_file_
		<< "source_frame_id,media_pts_ns,scheduled_release_us,received_at_us,"
		   "read_begin_us,read_end_us,detect_begin_us,detect_end_us,"
		   "result_ready_us,control_begin_us,control_end_us,overlay_begin_us,"
		   "overlay_end_us,publish_begin_us,publish_end_us,detected,center_x,"
		   "center_y,radius,area,frame_width,frame_height,publish_status\n";
	stream_pushes_file_
		<< "stream_push_id,source_frame_id,pushed_at_us,frame_age_us,"
		   "repeated_source_frame,flow_return\n";
	resources_file_
		<< "elapsed_ms,rss_kb,peak_rss_kb,threads,fd_count,user_cpu_ms,"
		   "system_cpu_ms\n";

	write_run_config(options, source_fps, expected_frame_count, command_line);
	enabled_ = true;
	writer_thread_ = std::thread(&PipelineMetrics::writer_loop, this);
	return true;
}

void PipelineMetrics::record_frame(FrameMetrics metrics) {
	if (!enabled_)
		return;

	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		if (frame_queue_.size() >= queue_capacity_) {
			++frame_rows_dropped_;
			return;
		}
		frame_queue_.push_back(std::move(metrics));
	}
	queue_cv_.notify_one();
}

void PipelineMetrics::record_stream_push(StreamPushMetrics metrics) {
	if (!enabled_)
		return;

	if (metrics.repeated_source_frame)
		++duplicate_stream_pushes_;
	if (metrics.flow_return != 0)
		++failed_stream_pushes_;

	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		if (stream_queue_.size() >= queue_capacity_) {
			++stream_rows_dropped_;
			return;
		}
		stream_queue_.push_back(std::move(metrics));
	}
	queue_cv_.notify_one();
}

void PipelineMetrics::record_latest_frame_overwrite() {
	if (enabled_)
		++latest_frame_overwrites_;
}

void PipelineMetrics::stop(std::uint64_t source_frames_read,
					   bool source_failed) {
	if (!enabled_)
		return;

	source_frames_read_ = source_frames_read;
	source_failed_ = source_failed;
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		stop_requested_ = true;
	}
	queue_cv_.notify_one();
	if (writer_thread_.joinable())
		writer_thread_.join();

	frames_file_.close();
	stream_pushes_file_.close();
	resources_file_.close();
	write_runtime_summary();
	enabled_ = false;
}

void PipelineMetrics::writer_loop() {
	sample_resources();
	auto next_resource_sample = start_time_ + std::chrono::seconds(1);
	for (;;) {
		std::deque<FrameMetrics> frames;
		std::deque<StreamPushMetrics> pushes;
		bool should_stop = false;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
				return stop_requested_ || !frame_queue_.empty() ||
					   !stream_queue_.empty();
			});
			frames.swap(frame_queue_);
			pushes.swap(stream_queue_);
			should_stop = stop_requested_ && frame_queue_.empty() &&
						  stream_queue_.empty();
		}

		for (const FrameMetrics &metrics : frames)
			write_frame(metrics);
		for (const StreamPushMetrics &metrics : pushes)
			write_stream_push(metrics);

		const auto now = PipelineClock::now();
		if (now >= next_resource_sample || should_stop) {
			sample_resources();
			while (next_resource_sample <= now)
				next_resource_sample += std::chrono::seconds(1);
		}
		if (should_stop)
			break;
	}
	frames_file_.flush();
	stream_pushes_file_.flush();
	resources_file_.flush();
}

void PipelineMetrics::write_frame(const FrameMetrics &metrics) {
	frames_file_ << metrics.meta.source_frame_id << ',';
	if (metrics.meta.media_pts)
		frames_file_ << metrics.meta.media_pts->count();
	frames_file_ << ',' << optional_elapsed_us(metrics.meta.scheduled_release)
				 << ',' << elapsed_us(metrics.meta.received_at) << ','
				 << elapsed_us(metrics.read_begin) << ','
				 << elapsed_us(metrics.read_end) << ','
				 << elapsed_us(metrics.detect_begin) << ','
				 << elapsed_us(metrics.detect_end) << ','
				 << elapsed_us(metrics.result_ready) << ','
				 << optional_elapsed_us(metrics.control_begin) << ','
				 << optional_elapsed_us(metrics.control_end) << ','
				 << optional_elapsed_us(metrics.overlay_begin) << ','
				 << optional_elapsed_us(metrics.overlay_end) << ','
				 << optional_elapsed_us(metrics.publish_begin) << ','
				 << optional_elapsed_us(metrics.publish_end) << ','
				 << (metrics.detected ? 1 : 0) << ',' << metrics.center_x << ','
				 << metrics.center_y << ',' << metrics.radius << ',' << metrics.area
				 << ',' << metrics.frame_width << ',' << metrics.frame_height << ','
				 << publish_status_name(metrics.publish_status) << '\n';
	++frame_rows_written_;
}

void PipelineMetrics::write_stream_push(const StreamPushMetrics &metrics) {
	const auto age_us = std::chrono::duration_cast<std::chrono::microseconds>(
		metrics.pushed_at - metrics.received_at);
	stream_pushes_file_ << metrics.stream_push_id << ','
					  << metrics.source_frame_id << ','
					  << elapsed_us(metrics.pushed_at) << ',' << age_us.count()
					  << ',' << (metrics.repeated_source_frame ? 1 : 0) << ','
					  << metrics.flow_return << '\n';
	++stream_rows_written_;
}

void PipelineMetrics::sample_resources() {
	const ResourceSample sample = read_resources();
	const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		PipelineClock::now() - start_time_);
	resources_file_ << elapsed_ms.count() << ',' << sample.rss_kb << ','
					<< sample.peak_rss_kb << ',' << sample.threads << ','
					<< sample.fd_count << ',' << sample.user_cpu_ms << ','
					<< sample.system_cpu_ms << '\n';
}

void PipelineMetrics::write_run_config(
	const Options &options, double source_fps,
	std::uint64_t expected_frame_count,
	const std::vector<std::string> &command_line) {
	std::ofstream config(std::filesystem::path(output_dir_) / "run_config.json",
					 std::ios::out | std::ios::trunc);
	config << "{\n  \"started_at_utc\": \"" << utc_now() << "\",\n"
		   << "  \"input_path\": \"" << json_escape(options.input_path)
		   << "\",\n  \"input_mode\": \""
		   << (options.input_path.empty() ? "camera" : "file") << "\",\n"
		   << "  \"replay_mode\": \"" << replay_mode_name(options.replay_mode)
		   << "\",\n  \"source_fps\": " << source_fps << ",\n"
		   << "  \"expected_frame_count\": " << expected_frame_count << ",\n"
		   << "  \"configured_width\": " << options.width << ",\n"
		   << "  \"configured_height\": " << options.height << ",\n"
		   << "  \"capture_fps_target\": " << options.fps << ",\n"
		   << "  \"stream_fps_target\": " << options.stream_fps << ",\n"
		   << "  \"stream_enabled\": "
		   << (options.stream_enabled ? "true" : "false") << ",\n"
		   << "  \"display_enabled\": "
		   << (options.show_preview ? "true" : "false") << ",\n"
		   << "  \"pan_tracking_enabled\": "
		   << (options.pan_tracking ? "true" : "false") << ",\n"
		   << "  \"metrics_queue_capacity\": "
		   << options.metrics_queue_capacity << ",\n  \"command_line\": [";
	for (std::size_t i = 0; i < command_line.size(); ++i) {
		if (i > 0)
			config << ',';
		config << "\"" << json_escape(command_line[i]) << "\"";
	}
	config << "]\n}\n";
}

void PipelineMetrics::write_runtime_summary() {
	std::ofstream summary(
		std::filesystem::path(output_dir_) / "runtime_summary.json",
		std::ios::out | std::ios::trunc);
	const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		PipelineClock::now() - start_time_);
	const std::uint64_t frame_dropped = frame_rows_dropped_.load();
	const std::uint64_t stream_dropped = stream_rows_dropped_.load();
	summary << "{\n"
			<< "  \"duration_ms\": " << duration_ms.count() << ",\n"
			<< "  \"source_frames_read\": " << source_frames_read_ << ",\n"
			<< "  \"source_failed\": "
			<< (source_failed_ ? "true" : "false") << ",\n"
			<< "  \"camera_upstream_drops\": null,\n"
			<< "  \"frame_rows_written\": " << frame_rows_written_.load()
			<< ",\n  \"frame_rows_dropped\": " << frame_dropped << ",\n"
			<< "  \"stream_rows_written\": " << stream_rows_written_.load()
			<< ",\n  \"stream_rows_dropped\": " << stream_dropped << ",\n"
			<< "  \"metrics_rows_dropped\": "
			<< frame_dropped + stream_dropped << ",\n"
			<< "  \"latest_frame_overwrites\": "
			<< latest_frame_overwrites_.load() << ",\n"
			<< "  \"duplicate_stream_pushes\": "
			<< duplicate_stream_pushes_.load() << ",\n"
			<< "  \"failed_stream_pushes\": "
			<< failed_stream_pushes_.load() << "\n}\n";
}

std::int64_t PipelineMetrics::elapsed_us(PipelineTimePoint time) const {
	return std::chrono::duration_cast<std::chrono::microseconds>(time - start_time_)
		.count();
}

std::string PipelineMetrics::optional_elapsed_us(
	const std::optional<PipelineTimePoint> &time) const {
	return time ? std::to_string(elapsed_us(*time)) : std::string();
}

} // namespace camera_detection
