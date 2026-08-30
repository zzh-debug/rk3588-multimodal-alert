#include "p2/streaming/video_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace p2 {
namespace {

bool finite_box(const FloatBox &box)
{
    return std::isfinite(box.left) && std::isfinite(box.top) &&
        std::isfinite(box.right) && std::isfinite(box.bottom);
}

}  // namespace

bool map_source_box_to_output(const FloatBox &source_box,
                              std::uint32_t source_width,
                              std::uint32_t source_height,
                              std::uint32_t output_width,
                              std::uint32_t output_height,
                              ImageRotation rotation,
                              OverlayBox *output_box,
                              std::string *error)
{
    if (output_box == nullptr || source_width == 0U || source_height == 0U ||
        output_width == 0U || output_height == 0U ||
        !finite_box(source_box) || source_box.right <= source_box.left ||
        source_box.bottom <= source_box.top) {
        if (error != nullptr)
            *error = "invalid source/output geometry for OSD mapping";
        return false;
    }

    const float source_w = static_cast<float>(source_width);
    const float source_h = static_cast<float>(source_height);
    const FloatBox clipped{
        std::clamp(source_box.left, 0.0F, source_w),
        std::clamp(source_box.top, 0.0F, source_h),
        std::clamp(source_box.right, 0.0F, source_w),
        std::clamp(source_box.bottom, 0.0F, source_h),
    };
    if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) {
        if (error != nullptr)
            *error = "source OSD box is outside the visible frame";
        return false;
    }

    FloatBox oriented;
    std::uint32_t oriented_width = source_width;
    std::uint32_t oriented_height = source_height;
    switch (rotation) {
    case ImageRotation::kNone:
        oriented = clipped;
        break;
    case ImageRotation::kClockwise90:
        oriented_width = source_height;
        oriented_height = source_width;
        oriented = {
            source_h - clipped.bottom,
            clipped.left,
            source_h - clipped.top,
            clipped.right,
        };
        break;
    case ImageRotation::kClockwise180:
        oriented = {
            source_w - clipped.right,
            source_h - clipped.bottom,
            source_w - clipped.left,
            source_h - clipped.top,
        };
        break;
    case ImageRotation::kClockwise270:
        oriented_width = source_height;
        oriented_height = source_width;
        oriented = {
            clipped.top,
            source_w - clipped.right,
            clipped.bottom,
            source_w - clipped.left,
        };
        break;
    }

    const float scale_x = static_cast<float>(output_width) /
        static_cast<float>(oriented_width);
    const float scale_y = static_cast<float>(output_height) /
        static_cast<float>(oriented_height);
    output_box->left = std::clamp(
        static_cast<int>(std::floor(oriented.left * scale_x)), 0,
        static_cast<int>(output_width) - 1);
    output_box->top = std::clamp(
        static_cast<int>(std::floor(oriented.top * scale_y)), 0,
        static_cast<int>(output_height) - 1);
    output_box->right = std::clamp(
        static_cast<int>(std::ceil(oriented.right * scale_x)),
        output_box->left + 1, static_cast<int>(output_width));
    output_box->bottom = std::clamp(
        static_cast<int>(std::ceil(oriented.bottom * scale_y)),
        output_box->top + 1, static_cast<int>(output_height));
    return true;
}

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
    std::string *error)
{
    if (overlay == nullptr ||
        detections.size() != matched_temperatures_c.size()) {
        if (error != nullptr)
            *error = "person detections and matched temperatures differ";
        return false;
    }
    VideoOverlay result;
    result.banner = alert_active ? "P2 ALERT" : "P2 MONITOR";
    result.banner_color = alert_active ? OverlayColor::red
                                       : OverlayColor::green;
    result.boxes.reserve(detections.size());
    for (std::size_t index = 0; index < detections.size(); ++index) {
        OverlayBox box;
        if (!map_source_box_to_output(
                detections[index].box, source_width, source_height,
                output_width, output_height, rotation, &box, error))
            return false;
        const bool matched =
            std::isfinite(matched_temperatures_c[index]);
        box.color = matched
            ? (alert_active ? OverlayColor::red : OverlayColor::yellow)
            : OverlayColor::green;
        std::ostringstream label;
        label << "PERSON "
              << static_cast<int>(std::lround(
                     detections[index].confidence * 100.0F))
              << '%';
        if (matched)
            label << ' ' << std::fixed << std::setprecision(1)
                  << matched_temperatures_c[index] << 'C';
        box.label = label.str();
        result.boxes.push_back(std::move(box));
    }
    *overlay = std::move(result);
    return true;
}

}  // namespace p2
