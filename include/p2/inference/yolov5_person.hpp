#pragma once

#include "p2/inference/letterbox.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace p2 {

struct QuantizedYoloTensorView {
    const std::int8_t *data = nullptr;
    std::size_t size = 0;
    std::uint32_t grid_width = 0;
    std::uint32_t grid_height = 0;
    std::int32_t zero_point = 0;
    float scale = 0.0F;
};

struct PersonDetection {
    FloatBox box;
    float confidence = 0.0F;
};

bool decode_yolov5_person(
    const std::vector<QuantizedYoloTensorView> &outputs,
    const LetterboxTransform &transform,
    float confidence_threshold,
    float nms_threshold,
    std::size_t maximum_detections,
    std::vector<PersonDetection> *detections,
    std::string *error);

}  // namespace p2
