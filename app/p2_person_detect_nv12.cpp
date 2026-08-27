#include "p2/inference/person_detector.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

bool parse_u32(const char *text, std::uint32_t *value)
{
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max())
        return false;
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool read_file(const std::string &path, std::vector<std::uint8_t> *bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes->assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 5 || argc > 7) {
        std::cerr << "Usage: " << argv[0]
                  << " MODEL FRAME.nv12 WIDTH HEIGHT [STRIDE] [ROTATION]\n";
        return EXIT_FAILURE;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    if (!parse_u32(argv[3], &width) || !parse_u32(argv[4], &height)) {
        std::cerr << "invalid frame dimensions\n";
        return EXIT_FAILURE;
    }
    stride = width;
    if (argc >= 6 && !parse_u32(argv[5], &stride)) {
        std::cerr << "invalid frame stride\n";
        return EXIT_FAILURE;
    }
    p2::ImageRotation rotation = p2::ImageRotation::kNone;
    if (argc == 7 && !p2::parse_image_rotation(argv[6], &rotation)) {
        std::cerr << "invalid rotation\n";
        return EXIT_FAILURE;
    }

    std::vector<std::uint8_t> frame;
    if (!read_file(argv[2], &frame)) {
        std::cerr << "cannot read NV12 frame: " << argv[2] << '\n';
        return EXIT_FAILURE;
    }
    const std::size_t expected = static_cast<std::size_t>(stride) * height *
        3U / 2U;
    if (frame.size() != expected) {
        std::cerr << "NV12 size mismatch: expected " << expected
                  << " bytes, got " << frame.size() << '\n';
        return EXIT_FAILURE;
    }

    p2::PersonDetectorConfig config;
    config.model_path = argv[1];
    config.rotation = rotation;
    p2::RknnPersonDetector detector(config);
    std::string error;
    if (!detector.initialize(&error)) {
        std::cerr << "detector initialization failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    p2::PersonInferenceResult result;
    if (!detector.infer_nv12(frame.data(), frame.size(), width, height,
                             stride, &result, &error)) {
        std::cerr << "inference failed: " << error << '\n';
        return EXIT_FAILURE;
    }

    const p2::PersonDetectorRuntimeInfo &runtime = detector.runtime_info();
    std::cout << std::fixed << std::setprecision(3)
              << "{\n"
              << "  \"application_commit\": \"" << P2_GIT_COMMIT
              << "\",\n"
              << "  \"rknn_api_version\": \"" << runtime.rknn_api_version
              << "\",\n"
              << "  \"rknn_driver_version\": \""
              << runtime.rknn_driver_version << "\",\n"
              << "  \"source_width\": " << width << ",\n"
              << "  \"source_height\": " << height << ",\n"
              << "  \"rotation\": \"" << p2::image_rotation_name(rotation)
              << "\",\n"
              << "  \"person_count\": " << result.detections.size() << ",\n"
              << "  \"rga_ms\": " << result.timing.rga_ms << ",\n"
              << "  \"rknn_ms\": " << result.timing.rknn_ms << ",\n"
              << "  \"postprocess_ms\": "
              << result.timing.postprocess_ms << ",\n"
              << "  \"total_ms\": " << result.timing.total_ms << ",\n"
              << "  \"persons\": [\n";
    for (std::size_t index = 0; index < result.detections.size(); ++index) {
        const p2::PersonDetection &detection = result.detections[index];
        std::cout << "    {\"confidence\": " << detection.confidence
                  << ", \"left\": " << detection.box.left
                  << ", \"top\": " << detection.box.top
                  << ", \"right\": " << detection.box.right
                  << ", \"bottom\": " << detection.box.bottom << "}"
                  << (index + 1U == result.detections.size() ? "" : ",")
                  << '\n';
    }
    std::cout << "  ]\n}\n";
    return EXIT_SUCCESS;
}
