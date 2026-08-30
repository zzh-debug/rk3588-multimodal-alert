#pragma once

#include "p2/inference/yolov5_person.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace p2 {

enum class OverlayColor : std::uint8_t {
    green = 1,
    red = 2,
    yellow = 3,
    white = 4,
};

struct OverlayBox {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    OverlayColor color = OverlayColor::green;
    std::string label;
};

struct VideoOverlay {
    std::string banner = "P2 MONITOR";
    OverlayColor banner_color = OverlayColor::green;
    std::vector<OverlayBox> boxes;
};

bool map_source_box_to_output(const FloatBox &source_box,
                              std::uint32_t source_width,
                              std::uint32_t source_height,
                              std::uint32_t output_width,
                              std::uint32_t output_height,
                              ImageRotation rotation,
                              OverlayBox *output_box,
                              std::string *error);

bool make_person_video_overlay(
    const std::vector<PersonDetection> &detections,
    const std::vector<float> &matched_temperatures_c,
    bool alert_active,
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t output_width,
    std::uint32_t output_height,
    ImageRotation rotation,
    VideoOverlay *overlay,
    std::string *error);

}  // namespace p2
