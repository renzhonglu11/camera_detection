#pragma once

#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <opencv2/opencv.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "utils/app_types.h"
#include "utils/pipeline_metrics.h"

namespace camera_detection {

class WebRtcStreamer
{
  public:
	explicit WebRtcStreamer(const Options &options,
						PipelineMetrics *metrics = nullptr);
	~WebRtcStreamer();

	bool create_offer(std::string &local_offer_sdp,
					  std::string &error_message,
					  int &http_status);
	bool set_answer(const std::string &remote_answer_sdp,
					std::string &error_message,
					int &http_status);
	PublishStatus publish_frame(const cv::Mat &frame, const FrameMeta &meta);
	void stop();
	bool add_remote_candidate(int sdp_mline_index,
							  const std::string &candidate,
							  std::string &error_message,
							  int &http_status);
	std::string local_candidates_json();

  private:
	struct EncoderSelection
	{
		std::string name;
		std::string pipeline_segment;
	};

	struct IceCandidate
	{
		unsigned int sdp_mline_index = 0;
		std::string candidate;
	};

	struct PublishedFrame
	{
		cv::Mat frame;
		FrameMeta meta;
		std::uint64_t generation = 0;
	};

	void release_webrtc_sink_pad_locked();
	bool validate_webrtc_runtime_locked(std::string &error_message);
	bool create_pipeline_locked(std::string &error_message);
	bool create_local_offer_locked(std::unique_lock<std::mutex> &lock,
								   std::string &local_offer_sdp,
								   std::string &error_message);
	bool set_remote_description_locked(const std::string &remote_sdp,
									   GstWebRTCSDPType type,
									   const char *description_name,
									   std::string &error_message);
	void teardown_pipeline_locked(std::unique_lock<std::mutex> &lock);
	void cleanup_terminated_session_locked(std::unique_lock<std::mutex> &lock);
	std::optional<EncoderSelection> select_encoder(std::string &error_message);
	void handle_ice_gathering_state_change(GObject *object);
	void handle_connection_state_change(GObject *object);
	void handle_ice_candidate(guint mlineindex, const gchar *candidate);
	void frame_loop();
	void push_frame_to_pipeline(const PublishedFrame &frame);
	void bus_loop(GstBus *bus);
	void mark_terminated(const std::string &reason);
	bool has_element_factory(const char *name) const;
	bool link_payloader_to_webrtc_locked(std::string &error_message);

	static void on_ice_gathering_state_notify(GObject *object,
											  GParamSpec *param_spec,
											  gpointer user_data);
	static void on_connection_state_notify(GObject *object,
										   GParamSpec *param_spec,
										   gpointer user_data);
	static void on_ice_candidate(GstElement *element,
								 guint mlineindex,
								 gchar *candidate,
								 gpointer user_data);

	int width_;
	int height_;
	int stream_fps_;
	WebRtcEncoder encoder_preference_;
	PipelineMetrics *metrics_ = nullptr;
	std::mutex frame_mutex_;
	std::condition_variable frame_cv_;
	PublishedFrame latest_frame_;
	std::uint64_t last_consumed_generation_ = 0;
	std::uint64_t next_generation_ = 1;
	std::uint64_t next_stream_push_id_ = 0;
	std::optional<std::uint64_t> last_pushed_source_frame_id_;
	bool frame_running_ = true;
	std::thread frame_thread_;
	GstElement *pipeline_ = nullptr;
	GstElement *appsrc_ = nullptr;
	GstElement *webrtcbin_ = nullptr;
	GstPad *webrtc_sink_pad_ = nullptr;
	std::mutex session_mutex_;
	std::condition_variable ice_state_cv_;
	bool ice_gathering_complete_ = false;
	bool session_active_ = false;
	bool awaiting_remote_answer_ = false;
	bool session_terminated_ = false;
	std::vector<IceCandidate> local_candidates_;
	std::string selected_encoder_name_;
	std::thread bus_thread_;
	std::atomic<bool> bus_running_{false};
};

} // namespace camera_detection
