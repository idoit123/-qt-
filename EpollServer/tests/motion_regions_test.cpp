#include "motion_regions.hpp"

#include <iostream>
#include <stdexcept>

using chat::MotionRegion;
using chat::MotionRegions;

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    try {
        const cv::Size size(320, 240);
        const std::vector<MotionRegion> head = {
            {{110, 50, 25, 25}, 600}, {{140, 52, 25, 25}, 600},
            {{120, 83, 30, 25}, 700}};
        MotionRegions merged;
        require(merged.update(head, size).empty(), "unconfirmed motion was displayed");
        auto boxes = merged.update(head, size);
        require(boxes.size() == 1, "adjacent head fragments were not grouped");
        require(boxes[0].contains(cv::Point(110, 50)) && boxes[0].contains(cv::Point(164, 107)),
                "merged box does not enclose all fragments");

        MotionRegions separate;
        const std::vector<MotionRegion> two = {{{25, 80, 40, 50}, 1900}, {{245, 80, 40, 50}, 1900}};
        separate.update(two, size);
        boxes = separate.update(two, size);
        require(boxes.size() == 2, "distant targets were combined");
        for (const auto &box : boxes) require(box.width == 40, "a box spans the gap between targets");

        MotionRegions noise;
        const std::vector<MotionRegion> flash = {{{60, 70, 45, 45}, 2000}};
        require(noise.update(flash, size).empty(), "single-frame flash was displayed");
        require(noise.update({}, size).empty(), "single-frame flash left a held box");
        require(noise.update(flash, size).empty(), "nonconsecutive flashes accumulated confirmation");
        noise.reset();
        const std::vector<MotionRegion> specks = {{{10, 10, 3, 3}, 4}, {{80, 90, 4, 4}, 9}};
        noise.update(specks, size);
        require(noise.update(specks, size).empty(), "tiny noise survived the area filter");

        MotionRegions smooth;
        const std::vector<MotionRegion> original = {{{40, 50, 50, 50}, 2400}};
        smooth.update(original, size); smooth.update(original, size);
        const std::vector<MotionRegion> shifted = {{{50, 54, 54, 50}, 2600}};
        boxes = smooth.update(shifted, size);
        require(boxes.size() == 1 && boxes[0].x > 40 && boxes[0].x < 50,
                "position change was not smoothed");
        require(boxes[0].width > 50 && boxes[0].width < 54, "size change was not smoothed");
        require(smooth.update({}, size).size() == 1, "one missed frame caused flicker");
        require(smooth.update({}, size).size() == 1, "two missed frames caused flicker");
        require(smooth.update({}, size).empty(), "held box did not expire");
        smooth.update(original, size); smooth.update(original, size); smooth.reset();
        require(smooth.update(original, size).empty(), "reset did not clear confirmed tracks");

        MotionRegions independent;
        require(independent.update(head, size).empty(), "state leaked between detector instances");
        std::cout << "MOTION_REGIONS_PASS: grouping, separation, flash/noise, smoothing, expiry, reset\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
