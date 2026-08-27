#pragma once

#include <cstdint>
#include <string>

namespace p2 {

enum class ImageRotation {
    kNone,
    kClockwise90,
    kClockwise180,
    kClockwise270,
};

struct FloatBox {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
};

struct LetterboxTransform {
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t oriented_width = 0;
    std::uint32_t oriented_height = 0;
    std::uint32_t model_width = 0;
    std::uint32_t model_height = 0;
    std::uint32_t resized_width = 0;
    std::uint32_t resized_height = 0;
    std::uint32_t pad_left = 0;
    std::uint32_t pad_top = 0;
    float scale_x = 0.0F;
    float scale_y = 0.0F;
    ImageRotation rotation = ImageRotation::kNone;
};

bool parse_image_rotation(const std::string &text, ImageRotation *rotation);
const char *image_rotation_name(ImageRotation rotation);

bool make_letterbox_transform(std::uint32_t source_width,
                              std::uint32_t source_height,
                              std::uint32_t model_width,
                              std::uint32_t model_height,
                              ImageRotation rotation,
                              LetterboxTransform *transform,
                              std::string *error);

FloatBox model_to_source_box(const LetterboxTransform &transform,
                             const FloatBox &model_box);

}  // namespace p2
