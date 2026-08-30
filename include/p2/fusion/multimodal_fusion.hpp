#pragma once

#include "p2/calibration/cross_spectral.hpp"
#include "p2/inference/yolov5_person.hpp"
#include "p2/thermal/mlx90640_math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace p2 {

struct MultimodalFusionConfig {
    float absolute_hot_temperature_c = 30.0F;
    float relative_hot_delta_c = 4.0F;
    std::size_t minimum_hot_pixels = 2;
    std::size_t minimum_finite_pixels = 700;
    double mapping_margin_px = 240.0;
    double minimum_thermal_overlap = 0.10;
    float minimum_person_confidence = 0.25F;
};

struct ThermalHotRegion {
    ThermalIndexRegion thermal_bounds;
    Rect2d mapped_visible_bounds;
    Rect2d expanded_visible_bounds;
    std::size_t pixel_count = 0;
    float mean_temperature_c = 0.0F;
    float max_temperature_c = 0.0F;
};

struct PersonThermalMatch {
    std::size_t person_index = 0;
    std::size_t hot_region_index = 0;
    double thermal_overlap = 0.0;
    float person_confidence = 0.0F;
    float max_temperature_c = 0.0F;
};

struct MultimodalFusionResult {
    float background_temperature_c = 0.0F;
    float activation_temperature_c = 0.0F;
    double effective_mapping_margin_px = 0.0;
    std::size_t finite_temperature_pixels = 0;
    std::vector<ThermalHotRegion> hot_regions;
    std::vector<PersonThermalMatch> matches;
    bool person_with_thermal_evidence = false;
};

bool analyze_multimodal_frame(
    const CrossSpectralCalibration &calibration,
    const std::array<float, kMlx90640Pixels> &temperature_c,
    const std::vector<PersonDetection> &people,
    const MultimodalFusionConfig &config,
    MultimodalFusionResult *result,
    std::string *error);

enum class AlertState {
    idle,
    candidate,
    active,
    cooldown,
};

enum class AlertTransition {
    none,
    activated,
    deactivated,
};

const char *to_string(AlertState state);
const char *to_string(AlertTransition transition);

struct AlertDebounceConfig {
    std::uint32_t confirmation_frames = 2;
    std::uint32_t release_frames = 3;
    std::uint64_t cooldown_ns = 1'000'000'000ULL;
};

struct AlertUpdate {
    AlertState state = AlertState::idle;
    AlertTransition transition = AlertTransition::none;
    bool active = false;
    std::uint32_t evidence_streak = 0;
    std::uint32_t miss_streak = 0;
    std::uint64_t cooldown_until_ns = 0;
};

class AlertDebouncer {
public:
    explicit AlertDebouncer(AlertDebounceConfig config = {});

    bool update(std::uint64_t timestamp_ns, bool has_evidence,
                AlertUpdate *update, std::string *error);
    void reset();

private:
    AlertDebounceConfig config_;
    AlertState state_ = AlertState::idle;
    std::uint32_t evidence_streak_ = 0;
    std::uint32_t miss_streak_ = 0;
    std::uint64_t cooldown_until_ns_ = 0;
    std::uint64_t last_timestamp_ns_ = 0;
    bool have_timestamp_ = false;
};

struct AlertEvent {
    std::uint64_t timestamp_ns = 0;
    std::uint32_t visible_sequence = 0;
    std::uint32_t thermal_sequence = 0;
    std::int64_t signed_skew_ns = 0;
    std::uint64_t absolute_skew_ns = 0;
    std::size_t person_count = 0;
    std::size_t hot_region_count = 0;
    std::size_t matched_pair_count = 0;
    float background_temperature_c = 0.0F;
    float activation_temperature_c = 0.0F;
    float maximum_matched_temperature_c = 0.0F;
    AlertUpdate alert;
};

}  // namespace p2
