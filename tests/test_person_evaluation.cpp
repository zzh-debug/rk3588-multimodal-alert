#include "p2/evaluation/person_dataset.hpp"
#include "p2/evaluation/person_metrics.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool near(double actual, double expected, double tolerance = 1.0e-5)
{
    return std::fabs(actual - expected) <= tolerance;
}

bool near_box(const p2::FloatBox &box,
              float left,
              float top,
              float right,
              float bottom)
{
    return near(box.left, left) && near(box.top, top) &&
        near(box.right, right) && near(box.bottom, bottom);
}

}  // namespace

int main()
{
    std::string error;
    p2::PersonPreviewGeometry geometry;
    if (!require(p2::make_person_preview_geometry(
                     3840, 2160, 960, p2::ImageRotation::kNone,
                     &geometry, &error),
                 "landscape preview geometry is valid") ||
        !require(geometry.preview_width == 960 &&
                     geometry.preview_height == 540,
                 "landscape preview preserves aspect ratio") ||
        !require(near_box(p2::source_to_person_preview_box(
                              geometry, {0.0F, 0.0F, 3840.0F, 2160.0F}),
                          0.0F, 0.0F, 960.0F, 540.0F),
                 "full source box maps to full landscape preview"))
        return EXIT_FAILURE;

    if (!require(p2::make_person_preview_geometry(
                     3840, 2160, 960, p2::ImageRotation::kClockwise90,
                     &geometry, &error),
                 "portrait preview geometry is valid") ||
        !require(geometry.preview_width == 540 &&
                     geometry.preview_height == 960,
                 "clockwise preview swaps axes") ||
        !require(near_box(p2::source_to_person_preview_box(
                              geometry, {100.0F, 200.0F, 500.0F, 800.0F}),
                          340.0F, 25.0F, 490.0F, 125.0F),
                 "clockwise preview box uses the same coordinate contract"))
        return EXIT_FAILURE;

    const std::string bmp_path = "/tmp/p2-person-preview-test.bmp";
    std::vector<std::uint8_t> nv12(12U, 128U);
    if (!require(p2::make_person_preview_geometry(
                     4, 2, 4, p2::ImageRotation::kNone,
                     &geometry, &error),
                 "small preview geometry is valid") ||
        !require(p2::write_nv12_person_preview_bmp(
                     bmp_path, nv12.data(), nv12.size(), 4,
                     geometry, &error),
                 "small NV12 preview writes as BMP"))
        return EXIT_FAILURE;
    std::ifstream bmp(bmp_path, std::ios::binary | std::ios::ate);
    const std::streamoff bmp_size = bmp
        ? static_cast<std::streamoff>(bmp.tellg())
        : static_cast<std::streamoff>(-1);
    bmp.seekg(0);
    char signature[2]{};
    bmp.read(signature, 2);
    std::remove(bmp_path.c_str());
    if (!require(bmp_size == 78 && signature[0] == 'B' && signature[1] == 'M',
                 "BMP header and padded size are deterministic"))
        return EXIT_FAILURE;

    const std::vector<std::string> images{"a", "b", "c"};
    const std::vector<p2::PersonGroundTruth> ground_truth{
        {"a", {0.0F, 0.0F, 100.0F, 100.0F}},
        {"b", {0.0F, 0.0F, 100.0F, 100.0F}},
        {"b", {200.0F, 200.0F, 300.0F, 300.0F}},
    };
    const std::vector<p2::PersonPrediction> predictions{
        {"a", {0.0F, 0.0F, 100.0F, 100.0F}, 0.90F},
        {"a", {150.0F, 150.0F, 200.0F, 200.0F}, 0.80F},
        {"b", {5.0F, 5.0F, 95.0F, 95.0F}, 0.70F},
        {"b", {200.0F, 200.0F, 300.0F, 300.0F}, 0.10F},
        {"c", {10.0F, 10.0F, 20.0F, 20.0F}, 0.60F},
    };
    p2::PersonEvaluationConfig config;
    p2::PersonEvaluationResult result;
    if (!require(p2::evaluate_person_predictions(
                     images, ground_truth, predictions, config,
                     &result, &error),
                 "evaluation accepts valid fixture") ||
        !require(result.images == 3 && result.positive_images == 2 &&
                     result.negative_images == 1,
                 "evaluation counts positive and negative images") ||
        !require(result.true_positives == 2 &&
                     result.false_positives == 2 &&
                     result.false_negatives == 1,
                 "greedy matching counts TP FP and FN") ||
        !require(near(result.precision, 0.5) &&
                     near(result.recall, 2.0 / 3.0) &&
                     near(result.f1, 4.0 / 7.0),
                 "precision recall and F1 are correct") ||
        !require(near(p2::person_box_iou(
                          {0.0F, 0.0F, 100.0F, 100.0F},
                          {50.0F, 50.0F, 150.0F, 150.0F}),
                      1.0 / 7.0),
                 "IoU is correct"))
        return EXIT_FAILURE;

    std::vector<p2::PersonPrediction> invalid = predictions;
    invalid.push_back({"unknown", {0.0F, 0.0F, 1.0F, 1.0F}, 0.9F});
    if (!require(!p2::evaluate_person_predictions(
                     images, ground_truth, invalid, config, &result, &error),
                 "unknown prediction image is rejected") ||
        !require(!p2::evaluate_person_predictions(
                     images, {}, predictions, config, &result, &error),
                 "empty ground truth is rejected"))
        return EXIT_FAILURE;

    std::cout << "person dataset and evaluation tests passed\n";
    return EXIT_SUCCESS;
}
