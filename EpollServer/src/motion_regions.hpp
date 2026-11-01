#pragma once

#include <opencv2/core.hpp>
#include <vector>

namespace chat {

struct MotionRegion {
    cv::Rect bounds;
    double foregroundArea;
};

// Spatial grouping and short-lived geometric association only: no neural
// network, weights, training data, or learned object/category detector.
class MotionRegions {
public:
    std::vector<cv::Rect> update(const std::vector<MotionRegion> &regions, cv::Size size);
    void reset() { tracks_.clear(); }

private:
    struct Track {
        cv::Rect2f observed;
        cv::Rect2f smoothed;
        unsigned hits = 1;
        unsigned misses = 0;
        bool confirmed = false;
    };
    std::vector<Track> tracks_;
};

} // namespace chat
