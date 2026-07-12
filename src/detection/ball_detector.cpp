#include "detection/ball_detector.h"

DetectionResult detect_ball(const cv::Mat &frame, cv::Mat *debug_mask) {
	DetectionResult best;
	cv::Mat hsv;
	cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);

	cv::Mat mask;
	cv::inRange(hsv, cv::Scalar(90, 100, 60), cv::Scalar(140, 255, 255), mask);

	cv::Mat kernel =
		cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));

	cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);
	cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

	if (debug_mask != nullptr) {
		*debug_mask = mask.clone();
	}

	std::vector<std::vector<cv::Point>> contours;
	cv::findContours(mask, contours, cv::RETR_EXTERNAL,
					 cv::CHAIN_APPROX_SIMPLE);

	for (const auto &contour : contours) {
		double area = cv::contourArea(contour);
		if (area < 300.0)
			continue;

		cv::Point2f center;
		float radius = 0.0f;
		cv::minEnclosingCircle(contour, center, radius);

		if (radius < 8.0f)
			continue;

		if (!best.detected || area > best.area) {
			best.detected = true;
			best.center = center;
			best.radius = radius;
			best.area = static_cast<float>(area);
		}
	}

	return best;
}
