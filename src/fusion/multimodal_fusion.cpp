#include "p2/fusion/multimodal_fusion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <vector>

namespace p2 {
namespace {

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr)
        *error = message;
}

bool valid_config(const MultimodalFusionConfig &config)
{
    return std::isfinite(config.absolute_hot_temperature_c) &&
        std::isfinite(config.relative_hot_delta_c) &&
        config.relative_hot_delta_c >= 0.0F &&
        config.minimum_hot_pixels > 0 &&
        config.minimum_hot_pixels <= kMlx90640Pixels &&
        config.minimum_finite_pixels > 0 &&
        config.minimum_finite_pixels <= kMlx90640Pixels &&
        std::isfinite(config.mapping_margin_px) &&
        config.mapping_margin_px >= 0.0 &&
        std::isfinite(config.minimum_thermal_overlap) &&
        config.minimum_thermal_overlap >= 0.0 &&
        config.minimum_thermal_overlap <= 1.0 &&
        std::isfinite(config.minimum_person_confidence) &&
        config.minimum_person_confidence >= 0.0F &&
        config.minimum_person_confidence <= 1.0F;
}

bool valid_person_box(const FloatBox &box)
{
    return std::isfinite(box.left) && std::isfinite(box.top) &&
        std::isfinite(box.right) && std::isfinite(box.bottom) &&
        box.right > box.left && box.bottom > box.top;
}

Rect2d expand_and_clip(const Rect2d &rectangle, double margin,
                       std::uint32_t width, std::uint32_t height)
{
    const double max_x = static_cast<double>(width - 1U);
    const double max_y = static_cast<double>(height - 1U);
    return {
        std::clamp(rectangle.x_min - margin, 0.0, max_x),
        std::clamp(rectangle.y_min - margin, 0.0, max_y),
        std::clamp(rectangle.x_max + margin, 0.0, max_x),
        std::clamp(rectangle.y_max + margin, 0.0, max_y),
    };
}

}  // namespace

bool analyze_multimodal_frame(
    const CrossSpectralCalibration &calibration,
    const std::array<float, kMlx90640Pixels> &temperature_c,
    const std::vector<PersonDetection> &people,
    const MultimodalFusionConfig &config,
    MultimodalFusionResult *result,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (result == nullptr) {
        set_error(error, "fusion result output pointer is null");
        return false;
    }
    if (!valid_config(config)) {
        set_error(error, "multimodal fusion configuration is invalid");
        return false;
    }
    if (!validate_cross_spectral_calibration(calibration, error))
        return false;

    std::vector<float> finite_temperatures;
    finite_temperatures.reserve(kMlx90640Pixels);
    for (const float value : temperature_c) {
        if (std::isfinite(value))
            finite_temperatures.push_back(value);
    }
    if (finite_temperatures.size() < config.minimum_finite_pixels) {
        set_error(error, "not enough finite thermal pixels for fusion");
        return false;
    }
    std::sort(finite_temperatures.begin(), finite_temperatures.end());
    const std::size_t middle = finite_temperatures.size() / 2U;
    float background = finite_temperatures[middle];
    if (finite_temperatures.size() % 2U == 0U) {
        background = (finite_temperatures[middle - 1U] +
                      finite_temperatures[middle]) * 0.5F;
    }
    const float activation = std::max(
        config.absolute_hot_temperature_c,
        background + config.relative_hot_delta_c);

    MultimodalFusionResult output;
    output.background_temperature_c = background;
    output.activation_temperature_c = activation;
    output.finite_temperature_pixels = finite_temperatures.size();
    output.effective_mapping_margin_px = std::max(
        config.mapping_margin_px,
        calibration.max_reprojection_error_px);

    std::array<bool, kMlx90640Pixels> hot{};
    std::array<bool, kMlx90640Pixels> visited{};
    for (std::size_t index = 0; index < temperature_c.size(); ++index) {
        hot[index] = std::isfinite(temperature_c[index]) &&
            temperature_c[index] >= activation;
    }

    constexpr std::array<int, 8> kColumnOffset{{-1, 0, 1, -1, 1, -1, 0, 1}};
    constexpr std::array<int, 8> kRowOffset{{-1, -1, -1, 0, 0, 1, 1, 1}};
    for (std::size_t seed = 0; seed < hot.size(); ++seed) {
        if (!hot[seed] || visited[seed])
            continue;
        std::queue<std::size_t> pending;
        std::vector<std::size_t> component;
        pending.push(seed);
        visited[seed] = true;
        while (!pending.empty()) {
            const std::size_t index = pending.front();
            pending.pop();
            component.push_back(index);
            const int column = static_cast<int>(index % kMlx90640Width);
            const int row = static_cast<int>(index / kMlx90640Width);
            for (std::size_t neighbor = 0;
                 neighbor < kColumnOffset.size(); ++neighbor) {
                const int next_column = column + kColumnOffset[neighbor];
                const int next_row = row + kRowOffset[neighbor];
                if (next_column < 0 || next_row < 0 ||
                    next_column >= static_cast<int>(kMlx90640Width) ||
                    next_row >= static_cast<int>(kMlx90640Height))
                    continue;
                const std::size_t next =
                    static_cast<std::size_t>(next_row) * kMlx90640Width +
                    static_cast<std::size_t>(next_column);
                if (hot[next] && !visited[next]) {
                    visited[next] = true;
                    pending.push(next);
                }
            }
        }
        if (component.size() < config.minimum_hot_pixels)
            continue;

        ThermalHotRegion region;
        region.thermal_bounds.first_column = kThermalColumns - 1U;
        region.thermal_bounds.first_row = kThermalRows - 1U;
        float temperature_sum = 0.0F;
        region.max_temperature_c = -std::numeric_limits<float>::infinity();
        for (const std::size_t index : component) {
            const std::uint32_t column = static_cast<std::uint32_t>(
                index % kMlx90640Width);
            const std::uint32_t row = static_cast<std::uint32_t>(
                index / kMlx90640Width);
            region.thermal_bounds.first_column = std::min(
                region.thermal_bounds.first_column, column);
            region.thermal_bounds.last_column = std::max(
                region.thermal_bounds.last_column, column);
            region.thermal_bounds.first_row = std::min(
                region.thermal_bounds.first_row, row);
            region.thermal_bounds.last_row = std::max(
                region.thermal_bounds.last_row, row);
            temperature_sum += temperature_c[index];
            region.max_temperature_c = std::max(
                region.max_temperature_c, temperature_c[index]);
        }
        region.pixel_count = component.size();
        region.mean_temperature_c = temperature_sum /
            static_cast<float>(component.size());

        MappedThermalRegion mapped;
        if (!map_thermal_region(calibration, region.thermal_bounds,
                                &mapped, error))
            return false;
        if (!mapped.intersects_visible_image)
            continue;
        region.mapped_visible_bounds = mapped.clipped_visible_bounds;
        region.expanded_visible_bounds = expand_and_clip(
            region.mapped_visible_bounds,
            output.effective_mapping_margin_px,
            calibration.visible_width, calibration.visible_height);
        output.hot_regions.push_back(region);
    }

    for (std::size_t person_index = 0; person_index < people.size();
         ++person_index) {
        const PersonDetection &person = people[person_index];
        if (person.confidence < config.minimum_person_confidence ||
            !valid_person_box(person.box))
            continue;
        const Rect2d person_bounds{
            person.box.left, person.box.top,
            person.box.right, person.box.bottom,
        };
        std::size_t best_region = 0;
        double best_overlap = 0.0;
        for (std::size_t region_index = 0;
             region_index < output.hot_regions.size(); ++region_index) {
            const double overlap = intersection_over_first_area(
                output.hot_regions[region_index].expanded_visible_bounds,
                person_bounds);
            if (overlap > best_overlap) {
                best_overlap = overlap;
                best_region = region_index;
            }
        }
        if (best_overlap >= config.minimum_thermal_overlap &&
            !output.hot_regions.empty()) {
            const ThermalHotRegion &region = output.hot_regions[best_region];
            output.matches.push_back({
                person_index,
                best_region,
                best_overlap,
                person.confidence,
                region.max_temperature_c,
            });
        }
    }
    output.person_with_thermal_evidence = !output.matches.empty();
    *result = std::move(output);
    return true;
}

const char *to_string(AlertState state)
{
    switch (state) {
    case AlertState::idle:
        return "idle";
    case AlertState::candidate:
        return "candidate";
    case AlertState::active:
        return "active";
    case AlertState::cooldown:
        return "cooldown";
    }
    return "idle";
}

const char *to_string(AlertTransition transition)
{
    switch (transition) {
    case AlertTransition::none:
        return "none";
    case AlertTransition::activated:
        return "activated";
    case AlertTransition::deactivated:
        return "deactivated";
    }
    return "none";
}

AlertDebouncer::AlertDebouncer(AlertDebounceConfig config)
    : config_(config)
{
    if (config_.confirmation_frames == 0 || config_.release_frames == 0)
        throw std::invalid_argument("alert debounce frame counts must be positive");
}

bool AlertDebouncer::update(std::uint64_t timestamp_ns, bool has_evidence,
                            AlertUpdate *update, std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (update == nullptr) {
        set_error(error, "alert update output pointer is null");
        return false;
    }
    if (have_timestamp_ && timestamp_ns < last_timestamp_ns_) {
        set_error(error, "alert timestamp moved backwards");
        return false;
    }
    have_timestamp_ = true;
    last_timestamp_ns_ = timestamp_ns;
    AlertTransition transition = AlertTransition::none;

    if (state_ == AlertState::cooldown &&
        timestamp_ns >= cooldown_until_ns_) {
        state_ = AlertState::idle;
        evidence_streak_ = 0;
        miss_streak_ = 0;
    }

    switch (state_) {
    case AlertState::idle:
        if (has_evidence) {
            evidence_streak_ = 1;
            state_ = AlertState::candidate;
            if (evidence_streak_ >= config_.confirmation_frames) {
                state_ = AlertState::active;
                transition = AlertTransition::activated;
            }
        } else {
            evidence_streak_ = 0;
        }
        break;
    case AlertState::candidate:
        if (has_evidence) {
            ++evidence_streak_;
            if (evidence_streak_ >= config_.confirmation_frames) {
                state_ = AlertState::active;
                miss_streak_ = 0;
                transition = AlertTransition::activated;
            }
        } else {
            state_ = AlertState::idle;
            evidence_streak_ = 0;
        }
        break;
    case AlertState::active:
        if (has_evidence) {
            miss_streak_ = 0;
        } else {
            ++miss_streak_;
            if (miss_streak_ >= config_.release_frames) {
                state_ = AlertState::cooldown;
                evidence_streak_ = 0;
                miss_streak_ = 0;
                if (timestamp_ns >
                    std::numeric_limits<std::uint64_t>::max() -
                        config_.cooldown_ns) {
                    cooldown_until_ns_ =
                        std::numeric_limits<std::uint64_t>::max();
                } else {
                    cooldown_until_ns_ = timestamp_ns + config_.cooldown_ns;
                }
                transition = AlertTransition::deactivated;
            }
        }
        break;
    case AlertState::cooldown:
        break;
    }

    update->state = state_;
    update->transition = transition;
    update->active = state_ == AlertState::active;
    update->evidence_streak = evidence_streak_;
    update->miss_streak = miss_streak_;
    update->cooldown_until_ns = cooldown_until_ns_;
    return true;
}

void AlertDebouncer::reset()
{
    state_ = AlertState::idle;
    evidence_streak_ = 0;
    miss_streak_ = 0;
    cooldown_until_ns_ = 0;
    last_timestamp_ns_ = 0;
    have_timestamp_ = false;
}

}  // namespace p2
