#include "motion_regions.hpp"

#include <algorithm>
#include <cmath>

namespace chat {
namespace {

double gapBetween(const cv::Rect &a, const cv::Rect &b)
{
    const int dx = std::max(0, std::max(a.x, b.x) - std::min(a.br().x, b.br().x));
    const int dy = std::max(0, std::max(a.y, b.y) - std::min(a.br().y, b.br().y));
    return std::hypot(dx, dy);
}

std::vector<cv::Rect> groupRegions(const std::vector<MotionRegion> &input, cv::Size size)
{
    const double pixels = static_cast<double>(size.area());
    const double minFragment = std::max(12.0, pixels * 0.00025);
    const double minGroup = std::max(100.0, pixels * 0.004);
    const double gap = std::clamp(std::min(size.width, size.height) * 0.075, 4.0, 18.0);
    std::vector<MotionRegion> groups;
    for (const auto &region : input) {
        const auto bounds = region.bounds & cv::Rect(0, 0, size.width, size.height);
        if (region.foregroundArea >= minFragment && !bounds.empty())
            groups.push_back({bounds, region.foregroundArea});
    }
    std::sort(groups.begin(), groups.end(), [](const MotionRegion &a, const MotionRegion &b) {
        return a.foregroundArea > b.foregroundArea;
    });
    // Bound the pairwise grouping cost for noisy masks, before clustering.
    if (groups.size() > 128) groups.resize(128);
    while (true) {
        std::size_t first = groups.size(), second = groups.size();
        double bestGap = gap + 1;
        for (std::size_t i = 0; i < groups.size(); ++i) {
            for (std::size_t j = i + 1; j < groups.size(); ++j) {
                const double distance = gapBetween(groups[i].bounds, groups[j].bounds);
                if (distance > gap || distance >= bestGap) continue;
                const auto combined = groups[i].bounds | groups[j].bounds;
                // Avoid growing sparse chains of specks into one huge box.
                if (groups[i].foregroundArea + groups[j].foregroundArea < combined.area() * 0.15)
                    continue;
                first = i; second = j; bestGap = distance;
            }
        }
        if (first == groups.size()) break;
        groups[first].bounds |= groups[second].bounds;
        groups[first].foregroundArea += groups[second].foregroundArea;
        groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(second));
    }
    std::sort(groups.begin(), groups.end(), [](const MotionRegion &a, const MotionRegion &b) {
        return a.foregroundArea > b.foregroundArea;
    });
    std::vector<cv::Rect> result;
    for (const auto &group : groups) {
        if (group.foregroundArea >= minGroup) result.push_back(group.bounds);
        if (result.size() == 32) break;
    }
    return result;
}

double overlap(const cv::Rect2f &a, const cv::Rect2f &b)
{
    const double intersection = (a & b).area();
    return intersection / std::max(1.0, a.area() + b.area() - intersection);
}

} // namespace

std::vector<cv::Rect> MotionRegions::update(const std::vector<MotionRegion> &regions, cv::Size size)
{
    const auto detections = groupRegions(regions, size);
    struct Match { double cost; std::size_t track; std::size_t detection; };
    std::vector<Match> matches;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        const auto &old = tracks_[i].observed;
        for (std::size_t j = 0; j < detections.size(); ++j) {
            const cv::Rect2f current(detections[j]);
            const double ratio = current.area() / std::max(1.0f, old.area());
            if (ratio < 0.25 || ratio > 4.0) continue;
            const double distance = cv::norm(cv::Point2f(old.x + old.width / 2, old.y + old.height / 2)
                                           - cv::Point2f(current.x + current.width / 2, current.y + current.height / 2));
            const double limit = std::max(12.0, 0.7 * std::max(std::hypot(old.width, old.height),
                                                               std::hypot(current.width, current.height)));
            const double iou = overlap(old, current);
            if (iou < 0.1 && distance > limit) continue;
            matches.push_back({distance / limit + 1.0 - iou, i, j});
        }
    }
    std::sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        return a.cost < b.cost;
    });
    std::vector<bool> usedTracks(tracks_.size(), false), usedDetections(detections.size(), false);
    for (const auto &match : matches) {
        if (usedTracks[match.track] || usedDetections[match.detection]) continue;
        auto &track = tracks_[match.track];
        const cv::Rect2f current(detections[match.detection]);
        constexpr float alpha = 0.45f;
        track.smoothed.x += alpha * (current.x - track.smoothed.x);
        track.smoothed.y += alpha * (current.y - track.smoothed.y);
        track.smoothed.width += alpha * (current.width - track.smoothed.width);
        track.smoothed.height += alpha * (current.height - track.smoothed.height);
        track.observed = current;
        track.hits = std::min(2U, track.hits + 1);
        track.misses = 0;
        track.confirmed = track.confirmed || track.hits >= 2;
        usedTracks[match.track] = true;
        usedDetections[match.detection] = true;
    }
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (!usedTracks[i]) { ++tracks_[i].misses; tracks_[i].hits = 0; }
    }
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [](const Track &track) {
        return (!track.confirmed && track.misses != 0) || track.misses > 2;
    }), tracks_.end());
    for (std::size_t i = 0; i < detections.size() && tracks_.size() < 32; ++i) {
        if (!usedDetections[i]) {
            const cv::Rect2f rect(detections[i]);
            tracks_.push_back({rect, rect, 1, 0, false});
        }
    }
    // Prefer current evidence over held boxes; suppress overlapping duplicates
    // when fragmented regions merge/split between frames.
    std::sort(tracks_.begin(), tracks_.end(), [](const Track &a, const Track &b) {
        if (a.misses != b.misses) return a.misses < b.misses;
        return a.smoothed.area() > b.smoothed.area();
    });
    std::vector<cv::Rect> result;
    for (const auto &track : tracks_) {
        if (!track.confirmed) continue;
        const auto &box = track.smoothed;
        const cv::Rect rect = cv::Rect(cvRound(box.x), cvRound(box.y),
                                      std::max(1, cvRound(box.width)), std::max(1, cvRound(box.height)))
                              & cv::Rect(0, 0, size.width, size.height);
        bool duplicate = rect.empty();
        for (const auto &existing : result) {
            const double contained = static_cast<double>((rect & existing).area())
                                     / std::max(1, std::min(rect.area(), existing.area()));
            if (overlap(cv::Rect2f(rect), cv::Rect2f(existing)) > 0.4 || contained > 0.65)
                duplicate = true;
        }
        if (!duplicate) result.push_back(rect);
        if (result.size() == 16) break;
    }
    return result;
}

} // namespace chat
