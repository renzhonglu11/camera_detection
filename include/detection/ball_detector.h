#pragma once

#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>

struct DetectionResult {
	bool detected = false;
	cv::Point2f center{};
	float radius = 0.0f;
	float area = 0.0f;
};

class BallDetector {
public:
	DetectionResult detect(const cv::Mat &frame,
						   cv::Mat *debug_mask = nullptr);

private:
	cv::Point2f pending_center_{};
	float pending_radius_ = 0.0f;
	int consecutive_detections_ = 0;
	bool tracking_ = false;
	cv::Point2f tracked_center_{};
	float tracked_radius_ = 0.0f;
	int missed_frames_ = 0;
};
