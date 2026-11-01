#pragma once

#include "protocol.hpp"
#include "motion_regions.hpp"
#include <opencv2/video/background_segm.hpp>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <memory>
#include <thread>

namespace chat {

// Owned by one TCP connection. Only the video worker accesses its contents;
// jobs retain it until completion, so disconnecting cannot leave dangling state.
struct MotionState {
    cv::Ptr<cv::BackgroundSubtractorMOG2> background;
    cv::Size sourceSize;
    unsigned frames = 0;
    MotionRegions regions;
};

// One processing worker; both the queue and each connection's outstanding work
// are bounded. The callback must post back to the connection's Reactor.
class VideoProcessor {
public:
    using Completion = std::function<void(Frame)>;
    VideoProcessor();
    ~VideoProcessor();
    bool submit(Frame frame, std::shared_ptr<MotionState> state, Completion done);
    void stop();

private:
    struct Job { Frame frame; std::shared_ptr<MotionState> state; Completion done; };
    void run();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace chat
