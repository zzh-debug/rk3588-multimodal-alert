#include "p2/inference/letterbox.hpp"

#include <algorithm>
#include <cmath>

namespace p2 {
namespace {

float clamp_value(float value, float low, float high)
{
    return std::max(low, std::min(value, high));
}

FloatBox oriented_to_source_box(const LetterboxTransform &transform,
                                const FloatBox &box)
{
    const float width = static_cast<float>(transform.source_width);
    const float height = static_cast<float>(transform.source_height);
    switch (transform.rotation) {
    case ImageRotation::kNone:
        return box;
    case ImageRotation::kClockwise90:
        return {box.top, height - box.right, box.bottom,
                height - box.left};
    case ImageRotation::kClockwise180:
        return {width - box.right, height - box.bottom,
                width - box.left, height - box.top};
    case ImageRotation::kClockwise270:
        return {width - box.bottom, box.left, width - box.top, box.right};
    }
    return {};
}

}  // namespace

bool parse_image_rotation(const std::string &text, ImageRotation *rotation)
{
    if (rotation == nullptr)
        return false;
    if (text == "0" || text == "none")
        *rotation = ImageRotation::kNone;
    else if (text == "90" || text == "90cw" || text == "cw90")
        *rotation = ImageRotation::kClockwise90;
    else if (text == "180" || text == "180cw")
        *rotation = ImageRotation::kClockwise180;
    else if (text == "270" || text == "270cw" || text == "90ccw")
        *rotation = ImageRotation::kClockwise270;
    else
        return false;
    return true;
}

const char *image_rotation_name(ImageRotation rotation)
{
    switch (rotation) {
    case ImageRotation::kNone:
        return "0";
    case ImageRotation::kClockwise90:
        return "90cw";
    case ImageRotation::kClockwise180:
        return "180";
    case ImageRotation::kClockwise270:
        return "270cw";
    }
    return "invalid";
}

bool make_letterbox_transform(std::uint32_t source_width,
                              std::uint32_t source_height,
                              std::uint32_t model_width,
                              std::uint32_t model_height,
                              ImageRotation rotation,
                              LetterboxTransform *transform,
                              std::string *error)
{
    if (transform == nullptr || source_width == 0 || source_height == 0 ||
        model_width == 0 || model_height == 0) {
        if (error != nullptr)
            *error = "letterbox dimensions and output must be non-zero";
        return false;
    }

    LetterboxTransform result;
    result.source_width = source_width;
    result.source_height = source_height;
    result.model_width = model_width;
    result.model_height = model_height;
    result.rotation = rotation;
    const bool swaps_axes = rotation == ImageRotation::kClockwise90 ||
        rotation == ImageRotation::kClockwise270;
    result.oriented_width = swaps_axes ? source_height : source_width;
    result.oriented_height = swaps_axes ? source_width : source_height;

    const double scale = std::min(
        static_cast<double>(model_width) / result.oriented_width,
        static_cast<double>(model_height) / result.oriented_height);
    result.resized_width = static_cast<std::uint32_t>(std::lround(
        static_cast<double>(result.oriented_width) * scale));
    result.resized_height = static_cast<std::uint32_t>(std::lround(
        static_cast<double>(result.oriented_height) * scale));
    result.resized_width = std::max(1U, std::min(model_width,
                                                 result.resized_width));
    result.resized_height = std::max(1U, std::min(model_height,
                                                  result.resized_height));
    result.pad_left = (model_width - result.resized_width) / 2U;
    result.pad_top = (model_height - result.resized_height) / 2U;
    result.scale_x = static_cast<float>(result.resized_width) /
        static_cast<float>(result.oriented_width);
    result.scale_y = static_cast<float>(result.resized_height) /
        static_cast<float>(result.oriented_height);
    *transform = result;
    return true;
}

FloatBox model_to_source_box(const LetterboxTransform &transform,
                             const FloatBox &model_box)
{
    const float oriented_width = static_cast<float>(transform.oriented_width);
    const float oriented_height = static_cast<float>(transform.oriented_height);
    FloatBox oriented{
        (model_box.left - static_cast<float>(transform.pad_left)) /
            transform.scale_x,
        (model_box.top - static_cast<float>(transform.pad_top)) /
            transform.scale_y,
        (model_box.right - static_cast<float>(transform.pad_left)) /
            transform.scale_x,
        (model_box.bottom - static_cast<float>(transform.pad_top)) /
            transform.scale_y,
    };
    oriented.left = clamp_value(oriented.left, 0.0F, oriented_width);
    oriented.right = clamp_value(oriented.right, 0.0F, oriented_width);
    oriented.top = clamp_value(oriented.top, 0.0F, oriented_height);
    oriented.bottom = clamp_value(oriented.bottom, 0.0F, oriented_height);

    FloatBox source = oriented_to_source_box(transform, oriented);
    source.left = clamp_value(source.left, 0.0F,
                              static_cast<float>(transform.source_width));
    source.right = clamp_value(source.right, 0.0F,
                               static_cast<float>(transform.source_width));
    source.top = clamp_value(source.top, 0.0F,
                             static_cast<float>(transform.source_height));
    source.bottom = clamp_value(source.bottom, 0.0F,
                                static_cast<float>(transform.source_height));
    if (source.left > source.right)
        std::swap(source.left, source.right);
    if (source.top > source.bottom)
        std::swap(source.top, source.bottom);
    return source;
}

}  // namespace p2
