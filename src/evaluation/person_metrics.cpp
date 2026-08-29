#include "p2/evaluation/person_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace p2 {
namespace {

bool valid_box(const FloatBox &box)
{
    return std::isfinite(box.left) && std::isfinite(box.top) &&
        std::isfinite(box.right) && std::isfinite(box.bottom) &&
        box.left >= 0.0F && box.top >= 0.0F &&
        box.right > box.left && box.bottom > box.top;
}

}  // namespace

float person_box_iou(const FloatBox &first, const FloatBox &second)
{
    if (!valid_box(first) || !valid_box(second))
        return 0.0F;
    const float left = std::max(first.left, second.left);
    const float top = std::max(first.top, second.top);
    const float right = std::min(first.right, second.right);
    const float bottom = std::min(first.bottom, second.bottom);
    const float intersection_width = std::max(0.0F, right - left);
    const float intersection_height = std::max(0.0F, bottom - top);
    const float intersection = intersection_width * intersection_height;
    const float first_area =
        (first.right - first.left) * (first.bottom - first.top);
    const float second_area =
        (second.right - second.left) * (second.bottom - second.top);
    const float union_area = first_area + second_area - intersection;
    return union_area > 0.0F ? intersection / union_area : 0.0F;
}

bool evaluate_person_predictions(
    const std::vector<std::string> &image_ids,
    const std::vector<PersonGroundTruth> &ground_truth,
    const std::vector<PersonPrediction> &predictions,
    const PersonEvaluationConfig &config,
    PersonEvaluationResult *result,
    std::string *error)
{
    if (result == nullptr || image_ids.empty()) {
        if (error != nullptr)
            *error = "evaluation requires a non-empty manifest and output";
        return false;
    }
    if (!std::isfinite(config.confidence_threshold) ||
        !std::isfinite(config.iou_threshold) ||
        config.confidence_threshold < 0.0F ||
        config.confidence_threshold > 1.0F ||
        config.iou_threshold <= 0.0F || config.iou_threshold > 1.0F) {
        if (error != nullptr)
            *error = "evaluation thresholds are outside valid ranges";
        return false;
    }

    std::unordered_set<std::string> known_images;
    for (const std::string &image_id : image_ids) {
        if (image_id.empty() || !known_images.insert(image_id).second) {
            if (error != nullptr)
                *error = "manifest image IDs must be non-empty and unique";
            return false;
        }
    }
    std::unordered_map<std::string, std::vector<FloatBox>> truth_by_image;
    for (const PersonGroundTruth &truth : ground_truth) {
        if (known_images.count(truth.image_id) == 0 || !valid_box(truth.box)) {
            if (error != nullptr)
                *error = "ground-truth row has unknown image or invalid box";
            return false;
        }
        truth_by_image[truth.image_id].push_back(truth.box);
    }
    if (ground_truth.empty()) {
        if (error != nullptr)
            *error = "ground-truth annotations contain no person instance";
        return false;
    }

    std::unordered_map<std::string, std::vector<PersonPrediction>>
        predictions_by_image;
    for (const PersonPrediction &prediction : predictions) {
        if (known_images.count(prediction.image_id) == 0 ||
            !valid_box(prediction.box) ||
            !std::isfinite(prediction.confidence) ||
            prediction.confidence < 0.0F || prediction.confidence > 1.0F) {
            if (error != nullptr)
                *error = "prediction row has unknown image or invalid values";
            return false;
        }
        if (prediction.confidence >= config.confidence_threshold)
            predictions_by_image[prediction.image_id].push_back(prediction);
    }

    PersonEvaluationResult evaluated;
    evaluated.images = image_ids.size();
    evaluated.ground_truth_instances = ground_truth.size();
    evaluated.per_image.reserve(image_ids.size());
    for (const std::string &image_id : image_ids) {
        const std::vector<FloatBox> &truths = truth_by_image[image_id];
        std::vector<PersonPrediction> accepted =
            predictions_by_image[image_id];
        std::sort(accepted.begin(), accepted.end(),
                  [](const PersonPrediction &first,
                     const PersonPrediction &second) {
                      return first.confidence > second.confidence;
                  });
        std::vector<bool> matched(truths.size(), false);
        PersonImageEvaluation image;
        image.image_id = image_id;
        image.ground_truth = truths.size();
        image.predictions = accepted.size();
        if (truths.empty())
            ++evaluated.negative_images;
        else
            ++evaluated.positive_images;

        for (const PersonPrediction &prediction : accepted) {
            float best_iou = -std::numeric_limits<float>::infinity();
            std::size_t best_index = truths.size();
            for (std::size_t index = 0; index < truths.size(); ++index) {
                if (matched[index])
                    continue;
                const float overlap = person_box_iou(prediction.box,
                                                     truths[index]);
                if (overlap > best_iou) {
                    best_iou = overlap;
                    best_index = index;
                }
            }
            if (best_index < truths.size() &&
                best_iou >= config.iou_threshold) {
                matched[best_index] = true;
                ++image.true_positives;
            } else {
                ++image.false_positives;
            }
        }
        image.false_negatives = static_cast<std::uint64_t>(std::count(
            matched.begin(), matched.end(), false));
        evaluated.accepted_predictions += image.predictions;
        evaluated.true_positives += image.true_positives;
        evaluated.false_positives += image.false_positives;
        evaluated.false_negatives += image.false_negatives;
        evaluated.per_image.push_back(image);
    }

    const double precision_denominator = static_cast<double>(
        evaluated.true_positives + evaluated.false_positives);
    const double recall_denominator = static_cast<double>(
        evaluated.true_positives + evaluated.false_negatives);
    evaluated.precision = precision_denominator > 0.0
        ? static_cast<double>(evaluated.true_positives) /
              precision_denominator
        : 0.0;
    evaluated.recall = recall_denominator > 0.0
        ? static_cast<double>(evaluated.true_positives) / recall_denominator
        : 0.0;
    const double f1_denominator = evaluated.precision + evaluated.recall;
    evaluated.f1 = f1_denominator > 0.0
        ? 2.0 * evaluated.precision * evaluated.recall / f1_denominator
        : 0.0;
    *result = evaluated;
    return true;
}

}  // namespace p2
