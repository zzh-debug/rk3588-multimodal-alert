#include "p2/inference/yolov5_person.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::int32_t kZeroPoint = -128;
constexpr float kScale = 1.0F / 255.0F;
constexpr std::size_t kChannelsPerAnchor = 85;

bool require(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

std::int8_t quantize(float value)
{
    const int quantized = static_cast<int>(std::lround(value / kScale)) +
        kZeroPoint;
    return static_cast<std::int8_t>(std::max(-128, std::min(127, quantized)));
}

void set_value(std::vector<std::int8_t> *tensor,
               std::size_t grid_size,
               std::size_t anchor,
               std::size_t channel,
               std::size_t cell,
               float value)
{
    (*tensor)[(anchor * kChannelsPerAnchor + channel) * grid_size + cell] =
        quantize(value);
}

}  // namespace

int main()
{
    p2::LetterboxTransform transform;
    std::string error;
    if (!p2::make_letterbox_transform(
            640, 640, 640, 640, p2::ImageRotation::kNone,
            &transform, &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    std::vector<std::int8_t> output8(3U * kChannelsPerAnchor * 80U * 80U,
                                     quantize(0.0F));
    std::vector<std::int8_t> output16(3U * kChannelsPerAnchor * 40U * 40U,
                                      quantize(0.0F));
    std::vector<std::int8_t> output32(3U * kChannelsPerAnchor * 20U * 20U,
                                      quantize(0.0F));
    const std::size_t grid_size = 20U * 20U;
    const std::size_t cell = 10U * 20U + 10U;
    for (std::size_t channel = 0; channel < 4U; ++channel)
        set_value(&output32, grid_size, 0U, channel, cell, 0.5F);
    set_value(&output32, grid_size, 0U, 4U, cell, 0.9F);
    set_value(&output32, grid_size, 0U, 5U, cell, 0.9F);

    // A lower-confidence, nearly identical second anchor must be removed by
    // person-only NMS.
    set_value(&output32, grid_size, 1U, 0U, cell, 0.5F);
    set_value(&output32, grid_size, 1U, 1U, cell, 0.5F);
    set_value(&output32, grid_size, 1U, 2U, cell,
              std::sqrt(116.0F / 156.0F) / 2.0F);
    set_value(&output32, grid_size, 1U, 3U, cell,
              std::sqrt(90.0F / 198.0F) / 2.0F);
    set_value(&output32, grid_size, 1U, 4U, cell, 0.8F);
    set_value(&output32, grid_size, 1U, 5U, cell, 0.8F);

    const std::vector<p2::QuantizedYoloTensorView> views{
        {output8.data(), output8.size(), 80, 80, kZeroPoint, kScale},
        {output16.data(), output16.size(), 40, 40, kZeroPoint, kScale},
        {output32.data(), output32.size(), 20, 20, kZeroPoint, kScale},
    };
    std::vector<p2::PersonDetection> detections;
    if (!require(p2::decode_yolov5_person(
                     views, transform, 0.25F, 0.45F, 64,
                     &detections, &error),
                 "synthetic YOLOv5 outputs decode") ||
        !require(detections.size() == 1U,
                 "person-only NMS suppresses the duplicate") ||
        !require(detections.front().confidence > 0.80F,
                 "combined person confidence is retained") ||
        !require(std::fabs(detections.front().box.left - 278.0F) < 2.0F &&
                     std::fabs(detections.front().box.top - 291.0F) < 2.0F &&
                     std::fabs(detections.front().box.right - 394.0F) < 2.0F &&
                     std::fabs(detections.front().box.bottom - 381.0F) < 2.0F,
                 "decoded box matches YOLOv5 anchor math"))
        return EXIT_FAILURE;

    std::vector<p2::QuantizedYoloTensorView> invalid = views;
    invalid.back().size = 1;
    if (!require(!p2::decode_yolov5_person(
                     invalid, transform, 0.25F, 0.45F, 64,
                     &detections, &error),
                 "undersized tensor is rejected"))
        return EXIT_FAILURE;

    std::cout << "YOLOv5 person postprocess tests passed\n";
    return EXIT_SUCCESS;
}
