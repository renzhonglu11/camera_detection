#pragma once

#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/opencv.hpp>

using namespace cv;

struct DetectionResult {
	bool detected = false;
	cv::Point2f center{};
	float radius = 0.0f;
	float area = 0.0f;
};

// DetectionResult detect_ball(const cv::Mat &frame);
DetectionResult detect_ball(const cv::Mat &frame,
							cv::Mat *debug_mask = nullptr);
