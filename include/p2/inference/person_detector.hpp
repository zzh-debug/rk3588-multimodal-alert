#pragma once

#include "p2/inference/letterbox.hpp"
#include "p2/inference/yolov5_person.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace p2 {

struct PersonDetectorConfig {
    std::string model_path;
    ImageRotation rotation = ImageRotation::kClockwise270;
    float confidence_threshold = 0.25F;
    float nms_threshold = 0.45F;
    std::size_t maximum_detections = 64;
};

struct PersonDetectorRuntimeInfo {
    std::string rknn_api_version;
    std::string rknn_driver_version;
    std::uint32_t model_width = 0;
    std::uint32_t model_height = 0;
    std::uint32_t model_channels = 0;
    std::uint32_t input_count = 0;
    std::uint32_t output_count = 0;
};

struct PersonInferenceTiming {
    double rga_ms = 0.0;
    double rknn_ms = 0.0;
    double postprocess_ms = 0.0;
    double total_ms = 0.0;
};

struct PersonInferenceResult {
    LetterboxTransform transform;
    PersonInferenceTiming timing;
    std::vector<PersonDetection> detections;
};

class RknnPersonDetector {
public:
    explicit RknnPersonDetector(PersonDetectorConfig config);
    ~RknnPersonDetector();

    RknnPersonDetector(const RknnPersonDetector &) = delete;
    RknnPersonDetector &operator=(const RknnPersonDetector &) = delete;

    bool initialize(std::string *error);
    bool infer_nv12(const std::uint8_t *data,
                    std::size_t size,
                    std::uint32_t width,
                    std::uint32_t height,
                    std::uint32_t bytes_per_line,
                    PersonInferenceResult *result,
                    std::string *error);
    const PersonDetectorRuntimeInfo &runtime_info() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace p2
