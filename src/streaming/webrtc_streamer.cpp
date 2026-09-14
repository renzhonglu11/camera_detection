#include "streaming/webrtc_streamer.h"

#include "utils/http_utils.h"

#include <gst/app/gstappsrc.h>
#include <gst/sdp/gstsdpmessage.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>

namespace camera_detection {

namespace {

void release_wrapped_frame(gpointer data) {
	delete static_cast<cv::Mat *>(data);
}

} // namespace

WebRtcStreamer::WebRtcStreamer(const Options &options,
						   PipelineMetrics *metrics)
	: width_(options.width), height_(options.height),
	  stream_fps_(options.stream_fps),
	  encoder_preference_(options.webrtc_encoder), metrics_(metrics) {
	frame_thread_ = std::thread(&WebRtcStreamer::frame_loop, this);
}

WebRtcStreamer::~WebRtcStreamer() { stop(); }

bool WebRtcStreamer::create_offer(std::string &local_offer_sdp,
								  std::string &error_message,
								  int &http_status) {
	std::unique_lock<std::mutex> lock(session_mutex_);
	cleanup_terminated_session_locked(lock);
	if (session_active_) {
		http_status = 409;
		error_message = "A browser peer is already connected";
		return false;
	}

	if (!create_pipeline_locked(error_message)) {
		http_status = 500;
		teardown_pipeline_locked(lock);
		return false;
	}

	if (!create_local_offer_locked(lock, local_offer_sdp, error_message)) {
		http_status = 500;
		teardown_pipeline_locked(lock);
		return false;
	}

	session_active_ = true;
	awaiting_remote_answer_ = true;
	http_status = 200;
	return true;
}

bool WebRtcStreamer::set_answer(const std::string &remote_answer_sdp,
								std::string &error_message, int &http_status) {
	std::unique_lock<std::mutex> lock(session_mutex_);
	cleanup_terminated_session_locked(lock);
	if (!session_active_ || !awaiting_remote_answer_) {
		http_status = 409;
		error_message = "There is no pending WebRTC offer";
		return false;
	}

	if (!set_remote_description_locked(remote_answer_sdp,
									   GST_WEBRTC_SDP_TYPE_ANSWER, "answer",
									   error_message)) {
		http_status = 500;
		teardown_pipeline_locked(lock);
		return false;
	}

	awaiting_remote_answer_ = false;
	http_status = 200;
	return true;
}

PublishStatus WebRtcStreamer::publish_frame(const cv::Mat &frame,
									 const FrameMeta &meta) {
	{
		std::lock_guard<std::mutex> lock(session_mutex_);
		if (!session_active_ || awaiting_remote_answer_ ||
			session_terminated_ || appsrc_ == nullptr) {
			return PublishStatus::NoSession;
		}
	}

	cv::Mat latest = frame.clone();
	if (latest.empty())
		return PublishStatus::CloneFailed;

	bool overwrote_unconsumed_frame = false;
	{
		std::lock_guard<std::mutex> lock(frame_mutex_);
		overwrote_unconsumed_frame =
			latest_frame_.generation != 0 &&
			last_consumed_generation_ < latest_frame_.generation;
		latest_frame_.frame = std::move(latest);
		latest_frame_.meta = meta;
		latest_frame_.generation = next_generation_++;
	}
	if (overwrote_unconsumed_frame && metrics_ != nullptr)
		metrics_->record_latest_frame_overwrite();
	frame_cv_.notify_one();
	return PublishStatus::Accepted;
}

void WebRtcStreamer::frame_loop() {
	const auto frame_period = std::chrono::nanoseconds(
		1'000'000'000LL / std::max(1, stream_fps_));
	std::unique_lock<std::mutex> lock(frame_mutex_);
	frame_cv_.wait(lock,
				   [this] { return !frame_running_ || !latest_frame_.frame.empty(); });
	auto next_frame_time = std::chrono::steady_clock::now();

	while (frame_running_) {
		const PublishedFrame frame = latest_frame_;
		last_consumed_generation_ = frame.generation;
		lock.unlock();
		push_frame_to_pipeline(frame);
		lock.lock();

		next_frame_time += frame_period;
		const auto now = std::chrono::steady_clock::now();
		if (next_frame_time <= now)
			next_frame_time = now + frame_period;

		frame_cv_.wait_until(lock, next_frame_time,
						 [this] { return !frame_running_; });
	}
}

void WebRtcStreamer::push_frame_to_pipeline(const PublishedFrame &published) {
	GstElement *appsrc = nullptr;
	{
		std::lock_guard<std::mutex> lock(session_mutex_);
		if (!session_active_ || awaiting_remote_answer_ ||
			session_terminated_ || appsrc_ == nullptr) {
			return;
		}

		appsrc = GST_ELEMENT(gst_object_ref(appsrc_));
	}

	const cv::Mat contiguous_frame = published.frame.isContinuous()
								 ? published.frame
								 : published.frame.clone();
	const std::size_t byte_count =
		contiguous_frame.total() * contiguous_frame.elemSize();
	auto *wrapped_frame = new cv::Mat(contiguous_frame);
	GstBuffer *buffer = gst_buffer_new_wrapped_full(
		GST_MEMORY_FLAG_READONLY, wrapped_frame->data, byte_count, 0, byte_count,
		wrapped_frame, &release_wrapped_frame);
	if (buffer == nullptr) {
		delete wrapped_frame;
		gst_object_unref(appsrc);
		if (metrics_ != nullptr) {
			const bool repeated = last_pushed_source_frame_id_ &&
				*last_pushed_source_frame_id_ == published.meta.source_frame_id;
			metrics_->record_stream_push(
				{next_stream_push_id_++, published.meta.source_frame_id,
				 published.meta.received_at, PipelineClock::now(), repeated, -1000});
			last_pushed_source_frame_id_ = published.meta.source_frame_id;
		}
		return;
	}
	GST_BUFFER_DURATION(buffer) =
		gst_util_uint64_scale_int(1, GST_SECOND, stream_fps_);

	const GstFlowReturn flow =
		gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
	const auto pushed_at = PipelineClock::now();
	gst_object_unref(appsrc);
	if (metrics_ != nullptr) {
		const bool repeated = last_pushed_source_frame_id_ &&
			*last_pushed_source_frame_id_ == published.meta.source_frame_id;
		metrics_->record_stream_push(
			{next_stream_push_id_++, published.meta.source_frame_id,
			 published.meta.received_at, pushed_at, repeated,
			 static_cast<int>(flow)});
		last_pushed_source_frame_id_ = published.meta.source_frame_id;
	}
	if (flow != GST_FLOW_OK)
		mark_terminated("WebRTC appsrc push failed");
}

void WebRtcStreamer::stop() {
	{
		std::lock_guard<std::mutex> lock(frame_mutex_);
		frame_running_ = false;
	}
	frame_cv_.notify_one();
	if (frame_thread_.joinable())
		frame_thread_.join();

	std::unique_lock<std::mutex> lock(session_mutex_);
	teardown_pipeline_locked(lock);
}

bool WebRtcStreamer::add_remote_candidate(int sdp_mline_index,
										  const std::string &candidate,
										  std::string &error_message,
										  int &http_status) {
	std::lock_guard<std::mutex> lock(session_mutex_);
	if (!session_active_ || webrtcbin_ == nullptr || session_terminated_) {
		http_status = 409;
		error_message = "There is no active WebRTC session";
		return false;
	}

	g_signal_emit_by_name(webrtcbin_, "add-ice-candidate",
						  static_cast<guint>(sdp_mline_index),
						  candidate.c_str());
	std::cout << "WebRTC remote candidate added for mline " << sdp_mline_index
			  << '\n';
	http_status = 200;
	return true;
}

std::string WebRtcStreamer::local_candidates_json() {
	std::lock_guard<std::mutex> lock(session_mutex_);
	std::ostringstream body;
	body << "{\"candidates\":[";
	for (std::size_t i = 0; i < local_candidates_.size(); ++i) {
		const auto &candidate = local_candidates_[i];
		if (i > 0)
			body << ',';

		body << "{\"sdpMLineIndex\":" << candidate.sdp_mline_index
			 << ",\"candidate\":\"" << json_escape(candidate.candidate)
			 << "\"}";
	}

	body << "],\"complete\":" << (ice_gathering_complete_ ? "true" : "false")
		 << "}\n";
	return body.str();
}

bool WebRtcStreamer::has_element_factory(const char *name) const {
	GstElementFactory *factory = gst_element_factory_find(name);
	if (factory == nullptr)
		return false;

	gst_object_unref(factory);
	return true;
}

bool WebRtcStreamer::validate_webrtc_runtime_locked(
	std::string &error_message) {
	if (!has_element_factory("webrtcbin")) {
		error_message = "GStreamer webrtcbin plugin is not available";
		return false;
	}

	if (!has_element_factory("nicesrc") || !has_element_factory("nicesink")) {
		error_message =
			"WebRTC requires libnice GStreamer elements, but nicesrc/nicesink "
			"are not available. Install the Raspberry Pi package "
			"gstreamer1.0-nice.";
		return false;
	}

	return true;
}

void WebRtcStreamer::release_webrtc_sink_pad_locked() {
	if (webrtc_sink_pad_ == nullptr)
		return;

	if (webrtcbin_ != nullptr)
		gst_element_release_request_pad(webrtcbin_, webrtc_sink_pad_);

	gst_object_unref(webrtc_sink_pad_);
	webrtc_sink_pad_ = nullptr;
}

bool WebRtcStreamer::link_payloader_to_webrtc_locked(
	std::string &error_message) {
	GstElement *rtp_caps = gst_bin_get_by_name(GST_BIN(pipeline_), "rtp_caps");
	if (rtp_caps == nullptr) {
		error_message = "WebRTC pipeline missing rtp_caps filter";
		return false;
	}

	GstPad *src_pad = gst_element_get_static_pad(rtp_caps, "src");
	gst_object_unref(rtp_caps);
	if (src_pad == nullptr) {
		error_message = "rtp_caps does not expose a src pad";
		return false;
	}

	GstPad *webrtc_sink_pad =
		gst_element_request_pad_simple(webrtcbin_, "sink_%u");
	if (webrtc_sink_pad == nullptr) {
		gst_object_unref(src_pad);
		error_message = "Failed to request a sink pad from webrtcbin";
		return false;
	}

	GObject *transceiver = nullptr;
	g_object_get(webrtc_sink_pad, "transceiver", &transceiver, nullptr);
	if (transceiver != nullptr) {
		g_object_set(transceiver, "direction",
					 GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, nullptr);
		g_object_unref(transceiver);
	}

	const GstPadLinkReturn link_result = gst_pad_link(src_pad, webrtc_sink_pad);
	gst_object_unref(src_pad);
	if (link_result != GST_PAD_LINK_OK) {
		gst_element_release_request_pad(webrtcbin_, webrtc_sink_pad);
		gst_object_unref(webrtc_sink_pad);
		error_message = "Failed to link RTP caps to webrtcbin";
		return false;
	}

	webrtc_sink_pad_ = webrtc_sink_pad;
	return true;
}

void WebRtcStreamer::on_ice_gathering_state_notify(GObject *object,
												   GParamSpec *,
												   gpointer user_data) {
	static_cast<WebRtcStreamer *>(user_data)->handle_ice_gathering_state_change(
		object);
}

void WebRtcStreamer::on_connection_state_notify(GObject *object, GParamSpec *,
												gpointer user_data) {
	static_cast<WebRtcStreamer *>(user_data)->handle_connection_state_change(
		object);
}

void WebRtcStreamer::on_ice_candidate(GstElement *, guint mlineindex,
									  gchar *candidate, gpointer user_data) {
	static_cast<WebRtcStreamer *>(user_data)->handle_ice_candidate(mlineindex,
																   candidate);
}

bool WebRtcStreamer::create_pipeline_locked(std::string &error_message) {
	if (!validate_webrtc_runtime_locked(error_message))
		return false;

	const auto encoder = select_encoder(error_message);
	if (!encoder)
		return false;

	std::ostringstream pipeline_description;
	pipeline_description
		<< "webrtcbin name=webrtc bundle-policy=max-bundle "
		   "stun-server=stun://stun.l.google.com:19302 "
		<< "appsrc name=video_source is-live=true do-timestamp=true "
		   "format=time "
		   "block=false max-buffers=2 "
		<< "caps=\"video/x-raw,format=BGR,width=" << width_
		<< ",height=" << height_ << ",framerate=" << stream_fps_ << "/1\" ! "
		<< "queue max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
		   "leaky=downstream ! "
		<< "videoconvert ! video/x-raw,format=I420 ! "
		<< "queue max-size-buffers=2 max-size-time=0 max-size-bytes=0 "
		   "leaky=downstream ! "
		<< encoder->pipeline_segment
		<< " ! video/x-h264,profile=constrained-baseline,level=(string)3.1,"
		   "stream-format=byte-stream,alignment=au ! "
		<< "h264parse config-interval=1 ! "
		<< "rtph264pay pt=96 config-interval=1 aggregate-mode=zero-latency ! "
		<< "capsfilter name=rtp_caps "
		   "caps=\"application/x-rtp,media=video,encoding-name=H264,payload=96,"
		   "clock-rate=90000\"";

	GError *parse_error = nullptr;
	pipeline_ =
		gst_parse_launch(pipeline_description.str().c_str(), &parse_error);
	if (parse_error != nullptr) {
		error_message = "Failed to build WebRTC pipeline: " +
						std::string(parse_error->message);
		g_error_free(parse_error);
		if (pipeline_ != nullptr)
			gst_object_unref(pipeline_);
		pipeline_ = nullptr;
		return false;
	}

	appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "video_source");
	webrtcbin_ = gst_bin_get_by_name(GST_BIN(pipeline_), "webrtc");
	if (appsrc_ == nullptr || webrtcbin_ == nullptr) {
		error_message = "WebRTC pipeline missing required elements";
		return false;
	}

	if (!link_payloader_to_webrtc_locked(error_message))
		return false;

	selected_encoder_name_ = encoder->name;
	ice_gathering_complete_ = false;
	session_terminated_ = false;

	g_signal_connect(webrtcbin_, "notify::ice-gathering-state",
					 G_CALLBACK(&WebRtcStreamer::on_ice_gathering_state_notify),
					 this);
	g_signal_connect(webrtcbin_, "notify::connection-state",
					 G_CALLBACK(&WebRtcStreamer::on_connection_state_notify),
					 this);
	g_signal_connect(webrtcbin_, "on-ice-candidate",
					 G_CALLBACK(&WebRtcStreamer::on_ice_candidate), this);

	if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
		GST_STATE_CHANGE_FAILURE) {
		error_message = "Failed to set WebRTC pipeline to PLAYING";
		return false;
	}

	GstBus *bus = gst_element_get_bus(pipeline_);
	if (bus != nullptr) {
		bus_running_ = true;
		bus_thread_ = std::thread(&WebRtcStreamer::bus_loop, this, bus);
	}

	std::cout << "WebRTC encoder: " << selected_encoder_name_ << '\n';
	return true;
}

bool WebRtcStreamer::set_remote_description_locked(
	const std::string &remote_sdp, GstWebRTCSDPType type,
	const char *description_name, std::string &error_message) {
	GstSDPMessage *sdp_message = nullptr;
	if (gst_sdp_message_new(&sdp_message) != GST_SDP_OK) {
		error_message = "Failed to allocate SDP message";
		return false;
	}

	const GstSDPResult parse_result = gst_sdp_message_parse_buffer(
		reinterpret_cast<const guint8 *>(remote_sdp.data()), remote_sdp.size(),
		sdp_message);
	if (parse_result != GST_SDP_OK) {
		gst_sdp_message_free(sdp_message);
		error_message =
			"Failed to parse browser SDP " + std::string(description_name);
		return false;
	}

	GstWebRTCSessionDescription *description =
		gst_webrtc_session_description_new(type, sdp_message);
	GstPromise *promise = gst_promise_new();
	g_signal_emit_by_name(webrtcbin_, "set-remote-description", description,
						  promise);
	const GstPromiseResult promise_result = gst_promise_wait(promise);
	gst_promise_unref(promise);
	gst_webrtc_session_description_free(description);
	if (promise_result == GST_PROMISE_RESULT_INTERRUPTED) {
		error_message = "Setting remote " + std::string(description_name) +
						" was interrupted";
		return false;
	}

	return true;
}

bool WebRtcStreamer::create_local_offer_locked(
	std::unique_lock<std::mutex> &lock, std::string &local_offer_sdp,
	std::string &error_message) {
	GstPromise *offer_promise = gst_promise_new();
	g_signal_emit_by_name(webrtcbin_, "create-offer", nullptr, offer_promise);
	const GstPromiseResult offer_result = gst_promise_wait(offer_promise);
	if (offer_result != GST_PROMISE_RESULT_REPLIED) {
		gst_promise_unref(offer_promise);
		error_message = "WebRTC create-offer did not produce a reply";
		return false;
	}

	const GstStructure *reply = gst_promise_get_reply(offer_promise);
	GstWebRTCSessionDescription *offer = nullptr;
	gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
					  &offer, nullptr);
	gst_promise_unref(offer_promise);
	if (offer == nullptr) {
		error_message = "WebRTC offer reply did not contain SDP";
		return false;
	}

	GstPromise *set_local_promise = gst_promise_new();
	g_signal_emit_by_name(webrtcbin_, "set-local-description", offer,
						  set_local_promise);
	gst_promise_wait(set_local_promise);
	gst_promise_unref(set_local_promise);

	const bool gathered =
		ice_state_cv_.wait_for(lock, std::chrono::seconds(10), [&] {
			return ice_gathering_complete_ || session_terminated_;
		});
	if (session_terminated_) {
		gst_webrtc_session_description_free(offer);
		error_message =
			"WebRTC session terminated while gathering ICE candidates";
		return false;
	}

	GstWebRTCSessionDescription *local_description = nullptr;
	if (!gathered)
		g_object_get(webrtcbin_, "pending-local-description",
					 &local_description, nullptr);
	if (local_description == nullptr)
		g_object_get(webrtcbin_, "local-description", &local_description,
					 nullptr);
	if (local_description == nullptr || local_description->sdp == nullptr) {
		gst_webrtc_session_description_free(offer);
		error_message = "WebRTC local description was not available";
		return false;
	}

	if (!gathered) {
		std::cerr << "WebRTC warning: ICE gathering did not reach complete "
					 "before timeout; continuing with current local description"
				  << '\n';
	}

	gchar *sdp_text = gst_sdp_message_as_text(local_description->sdp);
	if (sdp_text == nullptr) {
		gst_webrtc_session_description_free(local_description);
		gst_webrtc_session_description_free(offer);
		error_message = "Failed to serialize local SDP offer";
		return false;
	}

	local_offer_sdp = sdp_text;
	g_free(sdp_text);
	gst_webrtc_session_description_free(local_description);
	gst_webrtc_session_description_free(offer);
	return true;
}

void WebRtcStreamer::teardown_pipeline_locked(
	std::unique_lock<std::mutex> &lock) {
	session_active_ = false;
	awaiting_remote_answer_ = false;
	session_terminated_ = true;
	ice_gathering_complete_ = false;
	local_candidates_.clear();
	selected_encoder_name_.clear();
	ice_state_cv_.notify_all();

	if (pipeline_ != nullptr)
		gst_element_set_state(pipeline_, GST_STATE_NULL);

	bus_running_ = false;
	std::thread bus_thread = std::move(bus_thread_);
	GstElement *old_pipeline = pipeline_;
	pipeline_ = nullptr;
	lock.unlock();
	if (bus_thread.joinable())
		bus_thread.join();
	lock.lock();

	release_webrtc_sink_pad_locked();

	if (appsrc_ != nullptr) {
		gst_object_unref(appsrc_);
		appsrc_ = nullptr;
	}

	if (webrtcbin_ != nullptr) {
		gst_object_unref(webrtcbin_);
		webrtcbin_ = nullptr;
	}

	if (old_pipeline != nullptr)
		gst_object_unref(old_pipeline);
}

void WebRtcStreamer::cleanup_terminated_session_locked(
	std::unique_lock<std::mutex> &lock) {
	if (session_active_ && session_terminated_)
		teardown_pipeline_locked(lock);
}

std::optional<WebRtcStreamer::EncoderSelection>
WebRtcStreamer::select_encoder(std::string &error_message) {
	const auto try_factory = [](const char *name) -> bool {
		GstElementFactory *factory = gst_element_factory_find(name);
		if (factory == nullptr)
			return false;

		gst_object_unref(factory);
		return true;
	};

	auto make_selection =
		[&](const char *name) -> std::optional<EncoderSelection> {
		if (!try_factory(name))
			return std::nullopt;

		std::string segment = name;
		if (std::string(name) == "openh264enc") {
			segment += " bitrate=2000000 multi-thread=4 complexity=0";
		} else if (std::string(name) == "v4l2h264enc") {
			segment += " extra-controls=\"controls,video_b_frames=0,video_"
					   "gop_size=30,video_bitrate=2000000\"";
		}

		return EncoderSelection{name, segment};
	};

	switch (encoder_preference_) {
	case WebRtcEncoder::H264Hardware:
		if (const auto stateless = make_selection("v4l2slh264enc"))
			return stateless;
		if (const auto v4l2 = make_selection("v4l2h264enc"))
			return v4l2;
		error_message =
			"Requested --webrtc-encoder=h264-hw but no hardware H.264 encoder "
			"element was found";
		return std::nullopt;
	case WebRtcEncoder::OpenH264:
		if (const auto openh264 = make_selection("openh264enc"))
			return *openh264;
		error_message =
			"Requested --webrtc-encoder=openh264 but openh264enc was not found";
		return std::nullopt;
	case WebRtcEncoder::Auto:
		if (const auto stateless = make_selection("v4l2slh264enc"))
			return stateless;
		if (const auto openh264 = make_selection("openh264enc"))
			return *openh264;
		error_message =
			"No browser-compatible H.264 encoder was found. Checked "
			"v4l2slh264enc and openh264enc. v4l2h264enc is skipped in auto "
			"mode because it can freeze WebRTC transport on this target; use "
			"--webrtc-encoder=h264-hw to test it explicitly.";
		return std::nullopt;
	}

	error_message = "Unsupported WebRTC encoder selection";
	return std::nullopt;
}

void WebRtcStreamer::handle_ice_gathering_state_change(GObject *object) {
	GstWebRTCICEGatheringState state = GST_WEBRTC_ICE_GATHERING_STATE_NEW;
	g_object_get(object, "ice-gathering-state", &state, nullptr);
	if (state == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) {
		std::lock_guard<std::mutex> lock(session_mutex_);
		ice_gathering_complete_ = true;
		ice_state_cv_.notify_all();
	}
}

void WebRtcStreamer::handle_connection_state_change(GObject *object) {
	GstWebRTCPeerConnectionState state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
	g_object_get(object, "connection-state", &state, nullptr);
	if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
		state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED ||
		state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {
		mark_terminated("WebRTC peer disconnected");
	}
}

void WebRtcStreamer::handle_ice_candidate(guint mlineindex,
										  const gchar *candidate) {
	if (candidate == nullptr)
		return;

	std::lock_guard<std::mutex> lock(session_mutex_);
	local_candidates_.push_back(IceCandidate{mlineindex, candidate});
	std::cout << "WebRTC local candidate gathered for mline " << mlineindex
			  << '\n';
}

void WebRtcStreamer::bus_loop(GstBus *bus) {
	while (bus_running_) {
		GstMessage *message = gst_bus_timed_pop_filtered(
			bus, 200 * GST_MSECOND,
			static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS |
										GST_MESSAGE_WARNING));
		if (message == nullptr)
			continue;

		switch (GST_MESSAGE_TYPE(message)) {
		case GST_MESSAGE_ERROR: {
			GError *error = nullptr;
			gchar *debug_info = nullptr;
			gst_message_parse_error(message, &error, &debug_info);

			std::string text =
				error != nullptr ? error->message : "unknown GStreamer error";
			if (GST_MESSAGE_SRC(message) != nullptr) {
				text += " [";
				text += GST_OBJECT_NAME(GST_MESSAGE_SRC(message));
				text += "]";
			}
			if (debug_info != nullptr && debug_info[0] != '\0') {
				text += " debug: ";
				text += debug_info;
			}

			mark_terminated("GStreamer WebRTC error: " + text);
			if (error != nullptr)
				g_error_free(error);
			if (debug_info != nullptr)
				g_free(debug_info);
			break;
		}
		case GST_MESSAGE_EOS:
			mark_terminated("GStreamer WebRTC EOS");
			break;
		case GST_MESSAGE_WARNING: {
			GError *error = nullptr;
			gchar *debug_info = nullptr;
			gst_message_parse_warning(message, &error, &debug_info);
			if (error != nullptr) {
				std::cerr << "GStreamer warning: " << error->message << '\n';
				g_error_free(error);
			}
			if (debug_info != nullptr)
				g_free(debug_info);
			break;
		}
		default:
			break;
		}

		gst_message_unref(message);
	}

	gst_object_unref(bus);
}

void WebRtcStreamer::mark_terminated(const std::string &reason) {
	std::lock_guard<std::mutex> lock(session_mutex_);
	if (!session_terminated_)
		std::cerr << reason << '\n';

	session_terminated_ = true;
	ice_state_cv_.notify_all();
}

} // namespace camera_detection
