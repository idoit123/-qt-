#include "video_processor.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <utility>

namespace chat {
namespace {

// Check JPEG SOF before decoding, so a small compressed image cannot request
// an unbounded pixel allocation. This MVP accepts up to 1280x720, 8-bit JPEG.
bool boundedJpeg(const std::uint8_t *data, std::size_t size)
{
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
    std::size_t pos = 2;
    while (pos < size) {
        if (data[pos++] != 0xff) return false;
        while (pos < size && data[pos] == 0xff) ++pos;
        if (pos >= size) return false;
        const unsigned marker = data[pos++];
        if (marker == 0xda || marker == 0xd9) return false;
        if (pos + 2 > size) return false;
        const std::size_t length = (data[pos] << 8U) | data[pos + 1];
        if (length < 2 || length > size - pos) return false;
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            if (length < 8 || data[pos + 2] != 8) return false;
            const unsigned height = (data[pos + 3] << 8U) | data[pos + 4];
            const unsigned width = (data[pos + 5] << 8U) | data[pos + 6];
            return width > 0 && height > 0 && width <= 1280 && height <= 720;
        }
        pos += length;
    }
    return false;
}

std::string detectMotion(cv::Mat &image, MotionState &state, bool reset)
{
    constexpr unsigned warmupFrames = 20;
    if (reset || !state.background || state.sourceSize != image.size()) {
        state.background = cv::createBackgroundSubtractorMOG2(200, 25, false);
        state.sourceSize = image.size();
        state.frames = 0;
        state.regions.reset();
    }
    // Keep background-model memory and CPU bounded even for 720p inputs.
    const double scale = std::min({1.0, 320.0 / image.cols, 240.0 / image.rows});
    cv::Mat small, gray, mask;
    cv::resize(image, small, cv::Size(std::max(1, cvRound(image.cols * scale)),
                                    std::max(1, cvRound(image.rows * scale))));
    cv::cvtColor(small, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, gray, cv::Size(5, 5), 0);
    const bool learning = state.frames < warmupFrames;
    state.background->apply(gray, mask, learning ? 0.2 : 0.005);
    if (learning) {
        ++state.frames;
        return "warmup:" + std::to_string(state.frames) + "/20";
    }

    cv::morphologyEx(mask, mask, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::vector<MotionRegion> regions;
    for (const auto &contour : contours) {
        regions.push_back({cv::boundingRect(contour), cv::contourArea(contour)});
    }
    const auto boxes = state.regions.update(regions, small.size());
    const double sx = static_cast<double>(image.cols) / small.cols;
    const double sy = static_cast<double>(image.rows) / small.rows;
    for (const auto &box : boxes) {
        const cv::Rect rect(cvRound(box.x * sx), cvRound(box.y * sy),
                            cvRound(box.width * sx), cvRound(box.height * sy));
        cv::rectangle(image, rect & cv::Rect(0, 0, image.cols, image.rows),
                       cv::Scalar(0, 230, 0), 2);
    }
    return boxes.empty() ? "idle:0" : "motion:" + std::to_string(boxes.size());
}

Frame process(const Frame &request, MotionState &state)
{
    const auto error = [&request](const char *message) {
        Frame result = makeFrame(MessageType::VideoError, {}, {}, message);
        header(result)->data = header(request)->data;
        return result;
    };
    const auto size = header(request)->messageLength;
    const auto *data = request.data() + sizeof(LegacyHeader);
    if (!boundedJpeg(data, size)) return error("Expected JPEG, max 1280x720");
    try {
        const cv::Mat bytes(1, static_cast<int>(size), CV_8UC1,
                            const_cast<std::uint8_t *>(data));
        cv::Mat image = cv::imdecode(bytes, cv::IMREAD_COLOR | cv::IMREAD_IGNORE_ORIENTATION);
        if (image.empty() || image.cols > 1280 || image.rows > 720)
            return error("JPEG decode failed");
        const std::string status = detectMotion(image, state, field(*header(request), 32) == "reset");
        std::vector<std::uint8_t> jpeg;
        if (!cv::imencode(".jpg", image, jpeg, {cv::IMWRITE_JPEG_QUALITY, 80})
                || jpeg.size() > 512U * 1024U)
            return error("JPEG encode failed or output too large");
        Frame result = makeFrame(MessageType::VideoResponse, {}, {}, jpeg.data(), jpeg.size());
        header(result)->data = header(request)->data;
        std::fill(header(result)->data.begin() + 32, header(result)->data.end(), '\0');
        setField(*header(result), 32, status);
        return result;
    } catch (const cv::Exception &) {
        return error("OpenCV could not process this JPEG");
    }
}

} // namespace

VideoProcessor::VideoProcessor() : thread_(&VideoProcessor::run, this) {}
VideoProcessor::~VideoProcessor() { stop(); }

bool VideoProcessor::submit(Frame frame, std::shared_ptr<MotionState> state, Completion done)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || jobs_.size() >= 16) return false;
    jobs_.push_back({std::move(frame), std::move(state), std::move(done)});
    ready_.notify_one();
    return true;
}

void VideoProcessor::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    ready_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void VideoProcessor::run()
{
    cv::setNumThreads(1);
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job.done(process(job.frame, *job.state));
    }
}

} // namespace chat
