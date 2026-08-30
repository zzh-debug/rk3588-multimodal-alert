#include "p2/streaming/video_overlay.hpp"

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool same_box(const p2::OverlayBox &box, int left, int top,
              int right, int bottom)
{
    return box.left == left && box.top == top && box.right == right &&
        box.bottom == bottom;
}

}  // namespace

int main()
{
    p2::OverlayBox output;
    std::string error;
    if (!require(p2::map_source_box_to_output(
                     {0.0F, 0.0F, 3840.0F, 2160.0F}, 3840, 2160,
                     1080, 1920, p2::ImageRotation::kClockwise270,
                     &output, &error),
                 "full rotated frame maps") ||
        !require(same_box(output, 0, 0, 1080, 1920),
                 "full rotated frame fills portrait output") ||
        !require(p2::map_source_box_to_output(
                     {0.0F, 0.0F, 1920.0F, 1080.0F}, 3840, 2160,
                     1080, 1920, p2::ImageRotation::kClockwise270,
                     &output, &error),
                 "upper-left source quadrant maps") ||
        !require(same_box(output, 0, 960, 540, 1920),
                 "270-degree mapping uses the installed orientation") ||
        !require(p2::map_source_box_to_output(
                     {0.0F, 0.0F, 100.0F, 200.0F}, 3840, 2160,
                     1920, 1080, p2::ImageRotation::kNone,
                     &output, &error),
                 "unrotated box maps") ||
        !require(same_box(output, 0, 0, 50, 100),
                 "unrotated mapping scales both axes") ||
        !require(!p2::map_source_box_to_output(
                     {10.0F, 10.0F, 10.0F, 20.0F}, 3840, 2160,
                     1080, 1920, p2::ImageRotation::kClockwise270,
                     &output, &error),
                 "empty source box is rejected"))
        return EXIT_FAILURE;

    const std::vector<p2::PersonDetection> people{
        {{0.0F, 0.0F, 1920.0F, 1080.0F}, 0.874F},
    };
    p2::VideoOverlay overlay;
    if (!require(p2::make_person_video_overlay(
                     people, {36.54F}, true, 3840, 2160, 1080, 1920,
                     p2::ImageRotation::kClockwise270,
                     &overlay, &error),
                 "matched alert overlay builds") ||
        !require(overlay.banner == "P2 ALERT" &&
                     overlay.banner_color == p2::OverlayColor::red,
                 "alert banner is red") ||
        !require(overlay.boxes.size() == 1U &&
                     same_box(overlay.boxes.front(), 0, 960, 540, 1920),
                 "person box uses installed-orientation mapping") ||
        !require(overlay.boxes.front().color == p2::OverlayColor::red &&
                     overlay.boxes.front().label == "PERSON 87% 36.5C",
                 "matched alert label carries confidence and temperature") ||
        !require(p2::make_person_video_overlay(
                     people, {std::nanf("")}, false,
                     3840, 2160, 1080, 1920,
                     p2::ImageRotation::kClockwise270,
                     &overlay, &error),
                 "unmatched monitor overlay builds") ||
        !require(overlay.boxes.front().color == p2::OverlayColor::green &&
                     overlay.boxes.front().label == "PERSON 87%",
                 "unmatched person stays green without fake temperature") ||
        !require(!p2::make_person_video_overlay(
                     people, {}, false, 3840, 2160, 1080, 1920,
                     p2::ImageRotation::kClockwise270,
                     &overlay, &error),
                 "mismatched overlay vectors are rejected"))
        return EXIT_FAILURE;

    std::cout << "video overlay geometry tests passed\n";
    return EXIT_SUCCESS;
}
