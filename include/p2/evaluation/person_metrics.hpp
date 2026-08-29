#pragma once

#include "p2/inference/letterbox.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace p2 {

struct PersonGroundTruth {
    std::string image_id;
    FloatBox box;
};

struct PersonPrediction {
    std::string image_id;
    FloatBox box;
    float confidence = 0.0F;
};

struct PersonEvaluationConfig {
    float confidence_threshold = 0.25F;
    float iou_threshold = 0.50F;
};

struct PersonImageEvaluation {
    std::string image_id;
    std::uint64_t ground_truth = 0;
    std::uint64_t predictions = 0;
    std::uint64_t true_positives = 0;
    std::uint64_t false_positives = 0;
    std::uint64_t false_negatives = 0;
};

struct PersonEvaluationResult {
    std::uint64_t images = 0;
    std::uint64_t positive_images = 0;
    std::uint64_t negative_images = 0;
    std::uint64_t ground_truth_instances = 0;
    std::uint64_t accepted_predictions = 0;
    std::uint64_t true_positives = 0;
    std::uint64_t false_positives = 0;
    std::uint64_t false_negatives = 0;
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
    std::vector<PersonImageEvaluation> per_image;
};

float person_box_iou(const FloatBox &first, const FloatBox &second);

bool evaluate_person_predictions(
    const std::vector<std::string> &image_ids,
    const std::vector<PersonGroundTruth> &ground_truth,
    const std::vector<PersonPrediction> &predictions,
    const PersonEvaluationConfig &config,
    PersonEvaluationResult *result,
    std::string *error);

}  // namespace p2
