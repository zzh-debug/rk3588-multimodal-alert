#include "p2/inference/letterbox.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool near(float actual, float expected, float tolerance = 1.0e-3F)
{
    return std::fabs(actual - expected) <= tolerance;
}

bool near_box(const p2::FloatBox &actual, const p2::FloatBox &expected)
{
    return near(actual.left, expected.left) &&
        near(actual.top, expected.top) &&
        near(actual.right, expected.right) &&
        near(actual.bottom, expected.bottom);
}

}  // namespace

int main()
{
    std::string error;
    p2::LetterboxTransform transform;
    if (!require(p2::make_letterbox_transform(
                     3840, 2160, 640, 640,
                     p2::ImageRotation::kNone, &transform, &error),
                 "landscape letterbox is valid") ||
        !require(transform.resized_width == 640 &&
                     transform.resized_height == 360 &&
                     transform.pad_left == 0 && transform.pad_top == 140,
                 "landscape letterbox preserves aspect ratio") ||
        !require(near_box(p2::model_to_source_box(
                              transform, {0.0F, 140.0F, 640.0F, 500.0F}),
                          {0.0F, 0.0F, 3840.0F, 2160.0F}),
                 "landscape content maps back to the full source"))
        return EXIT_FAILURE;

    if (!require(p2::make_letterbox_transform(
                     3840, 2160, 640, 640,
                     p2::ImageRotation::kClockwise90, &transform, &error),
                 "clockwise portrait letterbox is valid") ||
        !require(transform.oriented_width == 2160 &&
                     transform.oriented_height == 3840 &&
                     transform.resized_width == 360 &&
                     transform.resized_height == 640 &&
                     transform.pad_left == 140 && transform.pad_top == 0,
                 "clockwise rotation swaps axes before letterbox") ||
        !require(near_box(p2::model_to_source_box(
                              transform, {140.0F, 0.0F, 500.0F, 640.0F}),
                          {0.0F, 0.0F, 3840.0F, 2160.0F}),
                 "rotated model content maps to raw 4K coordinates"))
        return EXIT_FAILURE;

    p2::ImageRotation parsed = p2::ImageRotation::kNone;
    if (!require(p2::parse_image_rotation("90cw", &parsed) &&
                     parsed == p2::ImageRotation::kClockwise90,
                 "90cw parses") ||
        !require(p2::parse_image_rotation("90ccw", &parsed) &&
                     parsed == p2::ImageRotation::kClockwise270,
                 "90ccw parses as clockwise 270") ||
        !require(!p2::parse_image_rotation("sideways", &parsed),
                 "unknown rotation is rejected") ||
        !require(!p2::make_letterbox_transform(
                     0, 2160, 640, 640, p2::ImageRotation::kNone,
                     &transform, &error),
                 "zero source dimension is rejected"))
        return EXIT_FAILURE;

    std::cout << "inference geometry tests passed\n";
    return EXIT_SUCCESS;
}
