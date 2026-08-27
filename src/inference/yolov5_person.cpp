#include "p2/inference/yolov5_person.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace p2 {
namespace {

constexpr std::uint32_t kClassCount = 80;
constexpr std::uint32_t kChannelsPerAnchor = 5U + kClassCount;
constexpr std::array<std::array<float, 6>, 3> kAnchors{{
    {{10.0F, 13.0F, 16.0F, 30.0F, 33.0F, 23.0F}},
    {{30.0F, 61.0F, 62.0F, 45.0F, 59.0F, 119.0F}},
    {{116.0F, 90.0F, 156.0F, 198.0F, 373.0F, 326.0F}},
}};

float dequantize(std::int8_t value, std::int32_t zero_point, float scale)
{
    return (static_cast<float>(value) - static_cast<float>(zero_point)) *
        scale;
}

float intersection_over_union(const FloatBox &first, const FloatBox &second)
{
    const float left = std::max(first.left, second.left);
    const float top = std::max(first.top, second.top);
    const float right = std::min(first.right, second.right);
    const float bottom = std::min(first.bottom, second.bottom);
    const float intersection = std::max(0.0F, right - left) *
        std::max(0.0F, bottom - top);
    const float first_area = std::max(0.0F, first.right - first.left) *
        std::max(0.0F, first.bottom - first.top);
    const float second_area = std::max(0.0F, second.right - second.left) *
        std::max(0.0F, second.bottom - second.top);
    const float union_area = first_area + second_area - intersection;
    return union_area > 0.0F ? intersection / union_area : 0.0F;
}

bool validate_threshold(float value)
{
    return std::isfinite(value) && value > 0.0F && value < 1.0F;
}

}  // namespace

bool decode_yolov5_person(
    const std::vector<QuantizedYoloTensorView> &outputs,
    const LetterboxTransform &transform,
    float confidence_threshold,
    float nms_threshold,
    std::size_t maximum_detections,
    std::vector<PersonDetection> *detections,
    std::string *error)
{
    if (detections == nullptr || outputs.size() != 3U ||
        maximum_detections == 0U || !validate_threshold(confidence_threshold) ||
        !validate_threshold(nms_threshold)) {
        if (error != nullptr)
            *error = "invalid YOLOv5 person decoder arguments";
        return false;
    }
    detections->clear();
    std::vector<PersonDetection> candidates;

    for (const QuantizedYoloTensorView &output : outputs) {
        if (output.data == nullptr || output.grid_width == 0 ||
            output.grid_height == 0 || !std::isfinite(output.scale) ||
            output.scale <= 0.0F) {
            if (error != nullptr)
                *error = "invalid quantized YOLOv5 output tensor";
            return false;
        }
        if (transform.model_width % output.grid_width != 0U ||
            transform.model_height % output.grid_height != 0U) {
            if (error != nullptr)
                *error = "YOLOv5 output grid does not divide model input";
            return false;
        }
        const std::uint32_t stride_x =
            transform.model_width / output.grid_width;
        const std::uint32_t stride_y =
            transform.model_height / output.grid_height;
        if (stride_x != stride_y ||
            (stride_x != 8U && stride_x != 16U && stride_x != 32U)) {
            if (error != nullptr)
                *error = "unsupported YOLOv5 output stride";
            return false;
        }
        const std::size_t grid_size =
            static_cast<std::size_t>(output.grid_width) * output.grid_height;
        const std::size_t required = 3U * kChannelsPerAnchor * grid_size;
        if (output.size < required) {
            if (error != nullptr)
                *error = "YOLOv5 output tensor is smaller than its shape";
            return false;
        }
        const std::size_t anchor_set = stride_x == 8U ? 0U :
            (stride_x == 16U ? 1U : 2U);

        for (std::size_t anchor = 0; anchor < 3U; ++anchor) {
            const std::size_t channel_base =
                anchor * kChannelsPerAnchor * grid_size;
            for (std::uint32_t row = 0; row < output.grid_height; ++row) {
                for (std::uint32_t column = 0;
                     column < output.grid_width; ++column) {
                    const std::size_t cell =
                        static_cast<std::size_t>(row) * output.grid_width +
                        column;
                    const float objectness = dequantize(
                        output.data[channel_base + 4U * grid_size + cell],
                        output.zero_point, output.scale);
                    if (objectness < confidence_threshold)
                        continue;
                    const float person_probability = dequantize(
                        output.data[channel_base + 5U * grid_size + cell],
                        output.zero_point, output.scale);
                    const float confidence = objectness * person_probability;
                    if (confidence < confidence_threshold)
                        continue;

                    const auto value = [&](std::size_t channel) {
                        return dequantize(
                            output.data[channel_base + channel * grid_size +
                                        cell],
                            output.zero_point, output.scale);
                    };
                    const float center_x =
                        (value(0U) * 2.0F - 0.5F +
                         static_cast<float>(column)) * stride_x;
                    const float center_y =
                        (value(1U) * 2.0F - 0.5F +
                         static_cast<float>(row)) * stride_y;
                    float width = value(2U) * 2.0F;
                    float height = value(3U) * 2.0F;
                    width = width * width *
                        kAnchors[anchor_set][anchor * 2U];
                    height = height * height *
                        kAnchors[anchor_set][anchor * 2U + 1U];
                    const FloatBox model_box{
                        center_x - width / 2.0F,
                        center_y - height / 2.0F,
                        center_x + width / 2.0F,
                        center_y + height / 2.0F,
                    };
                    PersonDetection detection;
                    detection.box = model_to_source_box(transform, model_box);
                    detection.confidence = confidence;
                    if (detection.box.right > detection.box.left &&
                        detection.box.bottom > detection.box.top)
                        candidates.push_back(detection);
                }
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const PersonDetection &first,
                 const PersonDetection &second) {
                  return first.confidence > second.confidence;
              });
    for (const PersonDetection &candidate : candidates) {
        bool suppressed = false;
        for (const PersonDetection &accepted : *detections) {
            if (intersection_over_union(candidate.box, accepted.box) >
                nms_threshold) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed)
            detections->push_back(candidate);
        if (detections->size() >= maximum_detections)
            break;
    }
    return true;
}

}  // namespace p2
