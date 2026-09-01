#include "detection/ball_detector.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr double kMinArea = 100.0;
constexpr float kMinRadius = 5.0f;
constexpr double kMinAcquisitionArea = 300.0;
constexpr float kMinAcquisitionRadius = 15.0f;
constexpr double kMinAcquisitionFillRatio = 0.30;
constexpr float kFrameBorderMargin = 2.0f;
constexpr int kBlueChannelMargin = 5;
constexpr int kRequiredDetections = 3;
constexpr int kMaxMissedFrames = 5;

struct Candidate {
	DetectionResult detection;
	double score = 0.0;
	double fill_ratio = 0.0;
};

float center_distance(const cv::Point2f &lhs, const cv::Point2f &rhs) {
	return std::hypot(lhs.x - rhs.x, lhs.y - rhs.y);
}

std::vector<Candidate> find_candidates(const cv::Mat &frame,
									   cv::Mat *debug_mask) {
	std::vector<Candidate> candidates;
	if (frame.empty())
		return candidates;

	cv::Mat hsv;
	cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);

	// Hue alone becomes unstable on the dark side of the ball. Use a wider HSV
	// range, then require the blue channel to dominate both green and red.
	cv::Mat hsv_mask;
	cv::inRange(hsv, cv::Scalar(90, 70, 25), cv::Scalar(140, 255, 255),
				hsv_mask);

	std::vector<cv::Mat> bgr_channels;
	cv::split(frame, bgr_channels);
	cv::Mat green_limit;
	cv::Mat red_limit;
	// Keep the absolute margin small: at low brightness, a genuinely blue pixel
	// may only differ from the other channels by a few intensity levels.
	cv::add(bgr_channels[1], cv::Scalar(kBlueChannelMargin), green_limit);
	cv::add(bgr_channels[2], cv::Scalar(kBlueChannelMargin), red_limit);

	cv::Mat blue_over_green;
	cv::Mat blue_over_red;
	cv::compare(bgr_channels[0], green_limit, blue_over_green, cv::CMP_GT);
	cv::compare(bgr_channels[0], red_limit, blue_over_red, cv::CMP_GT);

	cv::Mat blue_dominance_mask;
	cv::bitwise_and(blue_over_green, blue_over_red, blue_dominance_mask);

	cv::Mat mask;
	cv::bitwise_and(hsv_mask, blue_dominance_mask, mask);

	const cv::Mat close_kernel =
		cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
	const cv::Mat open_kernel =
		cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
	cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, close_kernel);
	cv::morphologyEx(mask, mask, cv::MORPH_OPEN, open_kernel);

	if (debug_mask != nullptr) {
		*debug_mask = mask.clone();
	}

	std::vector<std::vector<cv::Point>> contours;
	cv::findContours(mask, contours, cv::RETR_EXTERNAL,
					 cv::CHAIN_APPROX_SIMPLE);

	for (const auto &contour : contours) {
		const double area = cv::contourArea(contour);
		if (area < kMinArea)
			continue;

		std::vector<cv::Point> hull;
		cv::convexHull(contour, hull);
		const double hull_perimeter = cv::arcLength(hull, true);
		if (hull_perimeter <= 0.0)
			continue;

		const double circularity =
			4.0 * CV_PI * cv::contourArea(hull) /
			(hull_perimeter * hull_perimeter);

		const cv::RotatedRect bounding_rect = cv::minAreaRect(hull);
		const float short_side = std::min(bounding_rect.size.width,
									 bounding_rect.size.height);
		const float long_side = std::max(bounding_rect.size.width,
									bounding_rect.size.height);
		if (short_side <= 0.0f || long_side / short_side > 2.4f)
			continue;

		cv::Point2f center;
		float radius = 0.0f;
		cv::minEnclosingCircle(contour, center, radius);

		if (radius < kMinRadius)
			continue;

		// A clipped blue object at the image edge often looks like a small round
		// target. Wait until the whole candidate is visible before accepting it.
		if (center.x - radius <= kFrameBorderMargin ||
			center.y - radius <= kFrameBorderMargin ||
			center.x + radius >=
				static_cast<float>(frame.cols - 1) - kFrameBorderMargin ||
			center.y + radius >=
				static_cast<float>(frame.rows - 1) - kFrameBorderMargin) {
			continue;
		}

		const double fill_ratio = area / (CV_PI * radius * radius);
		const double shape_score =
			0.5 + 0.25 * std::clamp(circularity, 0.0, 1.0) +
			0.25 * std::clamp(fill_ratio, 0.0, 1.0);

		Candidate candidate;
		candidate.detection.detected = true;
		candidate.detection.center = center;
		candidate.detection.radius = radius;
		candidate.detection.area = static_cast<float>(area);
		candidate.score = area * shape_score;
		candidate.fill_ratio = fill_ratio;
		candidates.push_back(candidate);
	}

	return candidates;
}

const Candidate *acquisition_candidate(
	const std::vector<Candidate> &candidates) {
	const Candidate *best = nullptr;
	for (const Candidate &candidate : candidates) {
		if (candidate.detection.area < kMinAcquisitionArea ||
			candidate.detection.radius < kMinAcquisitionRadius ||
			candidate.fill_ratio < kMinAcquisitionFillRatio) {
			continue;
		}

		if (best == nullptr || candidate.score > best->score)
			best = &candidate;
	}

	return best;
}

const Candidate *tracked_candidate(const std::vector<Candidate> &candidates,
									   const cv::Point2f &tracked_center,
									   float tracked_radius) {
	const Candidate *best = nullptr;
	double best_score = 0.0;
	const float max_distance = std::max(60.0f, tracked_radius * 4.0f);

	for (const Candidate &candidate : candidates) {
		const float distance =
			center_distance(candidate.detection.center, tracked_center);
		if (distance > max_distance)
			continue;

		const double continuity_score =
			candidate.score / (1.0 + distance / 80.0);
		if (best == nullptr || continuity_score > best_score) {
			best = &candidate;
			best_score = continuity_score;
		}
	}

	return best;
}

} // namespace

DetectionResult BallDetector::detect(const cv::Mat &frame,
									 cv::Mat *debug_mask) {
	const std::vector<Candidate> candidates =
		find_candidates(frame, debug_mask);

	if (tracking_) {
		const Candidate *candidate =
			tracked_candidate(candidates, tracked_center_, tracked_radius_);
		if (candidate != nullptr) {
			missed_frames_ = 0;
			tracked_center_ = candidate->detection.center;
			tracked_radius_ = candidate->detection.radius;
			return candidate->detection;
		}

		++missed_frames_;
		if (missed_frames_ < kMaxMissedFrames)
			return {};

		tracking_ = false;
		consecutive_detections_ = 0;
	}

	const Candidate *candidate = acquisition_candidate(candidates);
	if (candidate == nullptr) {
		consecutive_detections_ = 0;
		return {};
	}

	const float confirmation_distance =
		std::max(40.0f, std::max(pending_radius_, candidate->detection.radius) *
						 3.0f);
	if (consecutive_detections_ > 0 &&
		center_distance(candidate->detection.center, pending_center_) <=
			confirmation_distance) {
		++consecutive_detections_;
	} else {
		consecutive_detections_ = 1;
	}

	pending_center_ = candidate->detection.center;
	pending_radius_ = candidate->detection.radius;
	if (consecutive_detections_ < kRequiredDetections)
		return {};

	tracking_ = true;
	missed_frames_ = 0;
	tracked_center_ = candidate->detection.center;
	tracked_radius_ = candidate->detection.radius;
	return candidate->detection;
}
