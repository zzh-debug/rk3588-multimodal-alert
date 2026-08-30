#include "p2/illumination/illumination_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace p2 {
namespace {

std::uint8_t percentile(const std::array<std::size_t, 256> &histogram,
                        std::size_t samples, double quantile)
{
    const std::size_t target = std::max<std::size_t>(
        1U, static_cast<std::size_t>(
            std::ceil(quantile * static_cast<double>(samples))));
    std::size_t cumulative = 0;
    for (std::size_t value = 0; value < histogram.size(); ++value) {
        cumulative += histogram[value];
        if (cumulative >= target)
            return static_cast<std::uint8_t>(value);
    }
    return 255;
}

bool valid_ratio(float value)
{
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

}  // namespace

bool analyze_nv12_luma(const std::uint8_t *data,
                       std::size_t size,
                       std::uint32_t width,
                       std::uint32_t height,
                       std::uint32_t stride,
                       LumaStatistics *statistics,
                       std::string *error)
{
    if (data == nullptr || statistics == nullptr || width < 16U ||
        height < 16U || stride < width ||
        stride > std::numeric_limits<std::size_t>::max() / height ||
        size < static_cast<std::size_t>(stride) * height) {
        if (error != nullptr)
            *error = "invalid NV12 luma frame";
        return false;
    }

    const std::uint32_t left = width / 10U;
    const std::uint32_t right = width - left;
    const std::uint32_t top = height / 10U;
    const std::uint32_t bottom = height - top;
    const std::uint32_t x_step = std::max<std::uint32_t>(
        1U, (right - left) / 64U);
    const std::uint32_t y_step = std::max<std::uint32_t>(
        1U, (bottom - top) / 36U);
    std::array<std::size_t, 256> histogram{};
    std::uint64_t sum = 0;
    std::size_t samples = 0;
    std::size_t dark_samples = 0;
    std::size_t bright_samples = 0;
    for (std::uint32_t y = top; y < bottom; y += y_step) {
        const std::uint8_t *row = data + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = left; x < right; x += x_step) {
            const std::uint8_t value = row[x];
            ++histogram[value];
            sum += value;
            ++samples;
            dark_samples += value <= 32U ? 1U : 0U;
            bright_samples += value >= 192U ? 1U : 0U;
        }
    }
    if (samples == 0U) {
        if (error != nullptr)
            *error = "NV12 luma sampler produced no samples";
        return false;
    }
    LumaStatistics result;
    result.valid = true;
    result.samples = samples;
    result.mean = static_cast<float>(
        static_cast<double>(sum) / static_cast<double>(samples));
    result.p10 = percentile(histogram, samples, 0.10);
    result.p50 = percentile(histogram, samples, 0.50);
    result.p90 = percentile(histogram, samples, 0.90);
    result.dark_fraction = static_cast<float>(dark_samples) /
        static_cast<float>(samples);
    result.bright_fraction = static_cast<float>(bright_samples) /
        static_cast<float>(samples);
    *statistics = result;
    return true;
}

const char *to_string(IlluminationState state)
{
    switch (state) {
    case IlluminationState::off: return "OFF";
    case IlluminationState::dark_pending: return "DARK_PENDING";
    case IlluminationState::ramp: return "RAMP";
    case IlluminationState::on: return "ON";
    case IlluminationState::hold: return "HOLD";
    case IlluminationState::cooldown: return "COOLDOWN";
    case IlluminationState::fault: return "FAULT";
    }
    return "UNKNOWN";
}

const char *to_string(RampDirection direction)
{
    switch (direction) {
    case RampDirection::none: return "none";
    case RampDirection::up: return "up";
    case RampDirection::down: return "down";
    }
    return "unknown";
}

IlluminationController::IlluminationController(IlluminationConfig config)
    : config_(config)
{
    if (config_.dark_p50_max > config_.ramp_stop_p50 ||
        config_.dark_p90_max > config_.ramp_stop_p90 ||
        !valid_ratio(config_.minimum_exposure_ratio) ||
        !valid_ratio(config_.minimum_gain_ratio) ||
        config_.target_brightness == 0U || config_.target_brightness > 255U ||
        config_.ramp_step == 0U ||
        config_.ramp_step > config_.target_brightness ||
        config_.dark_pending_ns == 0U || config_.ramp_step_ns == 0U ||
        config_.hold_ns == 0U || config_.cooldown_ns == 0U) {
        throw std::invalid_argument("invalid illumination configuration");
    }
}

void IlluminationController::enter_state(IlluminationState state,
                                         RampDirection direction,
                                         std::uint64_t timestamp_ns,
                                         bool *changed)
{
    if (state_ == state && ramp_direction_ == direction)
        return;
    state_ = state;
    ramp_direction_ = direction;
    state_since_ns_ = timestamp_ns;
    if (state == IlluminationState::ramp)
        last_ramp_ns_ = timestamp_ns;
    ++stats_.transitions;
    *changed = true;
}

void IlluminationController::fill_update(bool dark, bool trigger,
                                         bool changed,
                                         IlluminationUpdate *update) const
{
    update->state = state_;
    update->ramp_direction = ramp_direction_;
    update->desired_brightness = brightness_;
    update->dark_condition = dark;
    update->trigger_condition = trigger;
    update->state_changed = changed;
}

bool IlluminationController::update(
    const IlluminationObservation &observation,
    IlluminationUpdate *update,
    std::string *error)
{
    if (update == nullptr) {
        if (error != nullptr)
            *error = "illumination update output is null";
        return false;
    }
    ++stats_.updates;
    const bool invalid = observation.timestamp_ns == 0U ||
        !observation.luma.valid || observation.luma.samples == 0U ||
        !std::isfinite(observation.luma.mean) ||
        !observation.exposure.valid ||
        !valid_ratio(observation.exposure.exposure_ratio) ||
        !valid_ratio(observation.exposure.analogue_gain_ratio) ||
        (have_timestamp_ &&
         observation.timestamp_ns <= last_timestamp_ns_);
    if (invalid) {
        force_fault(update);
        if (error != nullptr)
            *error = "invalid or non-monotonic illumination telemetry";
        return false;
    }
    have_timestamp_ = true;
    last_timestamp_ns_ = observation.timestamp_ns;
    const bool exposure_elevated =
        observation.exposure.exposure_ratio >=
            config_.minimum_exposure_ratio ||
        observation.exposure.analogue_gain_ratio >=
            config_.minimum_gain_ratio;
    const bool dark = observation.luma.p50 <= config_.dark_p50_max &&
        observation.luma.p90 <= config_.dark_p90_max && exposure_elevated;
    const bool bright_enough =
        observation.luma.p50 >= config_.ramp_stop_p50 ||
        observation.luma.p90 >= config_.ramp_stop_p90;
    const bool trigger = dark && observation.sustained_thermal_target;
    bool changed = false;
    const std::uint32_t previous_brightness = brightness_;

    switch (state_) {
    case IlluminationState::off:
        brightness_ = 0;
        if (trigger)
            enter_state(IlluminationState::dark_pending,
                        RampDirection::none,
                        observation.timestamp_ns, &changed);
        break;
    case IlluminationState::dark_pending:
        brightness_ = 0;
        if (!trigger) {
            enter_state(IlluminationState::off, RampDirection::none,
                        observation.timestamp_ns, &changed);
        } else if (observation.timestamp_ns - state_since_ns_ >=
                   config_.dark_pending_ns) {
            brightness_ = std::min(config_.ramp_step,
                                   config_.target_brightness);
            enter_state(IlluminationState::ramp, RampDirection::up,
                        observation.timestamp_ns, &changed);
        }
        break;
    case IlluminationState::ramp:
        if (ramp_direction_ == RampDirection::up) {
            if (!observation.sustained_thermal_target) {
                enter_state(IlluminationState::hold, RampDirection::none,
                            observation.timestamp_ns, &changed);
            } else if (bright_enough ||
                       brightness_ >= config_.target_brightness) {
                enter_state(IlluminationState::on, RampDirection::none,
                            observation.timestamp_ns, &changed);
            } else if (observation.timestamp_ns - last_ramp_ns_ >=
                       config_.ramp_step_ns) {
                const std::uint64_t steps =
                    (observation.timestamp_ns - last_ramp_ns_) /
                    config_.ramp_step_ns;
                const std::uint64_t increment =
                    steps * config_.ramp_step;
                brightness_ = static_cast<std::uint32_t>(std::min<
                    std::uint64_t>(config_.target_brightness,
                                  brightness_ + increment));
                last_ramp_ns_ += steps * config_.ramp_step_ns;
                if (brightness_ >= config_.target_brightness)
                    enter_state(IlluminationState::on,
                                RampDirection::none,
                                observation.timestamp_ns, &changed);
            }
        } else if (ramp_direction_ == RampDirection::down) {
            if (observation.sustained_thermal_target && brightness_ > 0U) {
                enter_state(IlluminationState::on,
                            RampDirection::none,
                            observation.timestamp_ns, &changed);
            } else if (observation.timestamp_ns - last_ramp_ns_ >=
                       config_.ramp_step_ns) {
                const std::uint64_t steps =
                    (observation.timestamp_ns - last_ramp_ns_) /
                    config_.ramp_step_ns;
                const std::uint64_t decrement =
                    steps * config_.ramp_step;
                brightness_ = decrement >= brightness_
                    ? 0U
                    : brightness_ - static_cast<std::uint32_t>(decrement);
                last_ramp_ns_ += steps * config_.ramp_step_ns;
                if (brightness_ == 0U)
                    enter_state(IlluminationState::cooldown,
                                RampDirection::none,
                                observation.timestamp_ns, &changed);
            }
        } else {
            force_fault(update);
            if (error != nullptr)
                *error = "illumination ramp has no direction";
            return false;
        }
        break;
    case IlluminationState::on:
        if (!observation.sustained_thermal_target)
            enter_state(IlluminationState::hold, RampDirection::none,
                        observation.timestamp_ns, &changed);
        break;
    case IlluminationState::hold:
        if (observation.sustained_thermal_target) {
            enter_state(IlluminationState::on, RampDirection::none,
                        observation.timestamp_ns, &changed);
        } else if (observation.timestamp_ns - state_since_ns_ >=
                   config_.hold_ns) {
            if (brightness_ == 0U) {
                enter_state(IlluminationState::cooldown,
                            RampDirection::none,
                            observation.timestamp_ns, &changed);
            } else {
                brightness_ = brightness_ <= config_.ramp_step
                    ? 0U : brightness_ - config_.ramp_step;
                if (brightness_ == 0U) {
                    enter_state(IlluminationState::cooldown,
                                RampDirection::none,
                                observation.timestamp_ns, &changed);
                } else {
                    enter_state(IlluminationState::ramp,
                                RampDirection::down,
                                observation.timestamp_ns, &changed);
                }
            }
        }
        break;
    case IlluminationState::cooldown:
        brightness_ = 0;
        if (observation.timestamp_ns - state_since_ns_ >=
            config_.cooldown_ns) {
            enter_state(trigger ? IlluminationState::dark_pending
                                : IlluminationState::off,
                        RampDirection::none,
                        observation.timestamp_ns, &changed);
        }
        break;
    case IlluminationState::fault:
        brightness_ = 0;
        break;
    }

    if (previous_brightness == 0U && brightness_ > 0U)
        ++stats_.activations;
    if (previous_brightness > 0U && brightness_ == 0U)
        ++stats_.deactivations;
    stats_.maximum_brightness = std::max(stats_.maximum_brightness,
                                         brightness_);
    fill_update(dark, trigger, changed, update);
    return true;
}

void IlluminationController::shutdown(IlluminationUpdate *update)
{
    const bool changed = state_ != IlluminationState::off ||
        ramp_direction_ != RampDirection::none || brightness_ != 0U;
    if (brightness_ > 0U)
        ++stats_.deactivations;
    state_ = IlluminationState::off;
    ramp_direction_ = RampDirection::none;
    brightness_ = 0U;
    if (changed) {
        ++stats_.transitions;
        state_since_ns_ = last_timestamp_ns_;
    }
    if (update != nullptr)
        fill_update(false, false, changed, update);
}

void IlluminationController::force_fault(IlluminationUpdate *update)
{
    const bool was_active = brightness_ > 0U;
    const bool changed = state_ != IlluminationState::fault;
    state_ = IlluminationState::fault;
    ramp_direction_ = RampDirection::none;
    brightness_ = 0;
    if (changed) {
        ++stats_.transitions;
        ++stats_.faults;
    }
    if (was_active)
        ++stats_.deactivations;
    if (update != nullptr)
        fill_update(false, false, changed, update);
}

void IlluminationController::reset()
{
    state_ = IlluminationState::off;
    ramp_direction_ = RampDirection::none;
    brightness_ = 0;
    state_since_ns_ = 0;
    last_ramp_ns_ = 0;
    last_timestamp_ns_ = 0;
    have_timestamp_ = false;
    stats_ = {};
}

}  // namespace p2
