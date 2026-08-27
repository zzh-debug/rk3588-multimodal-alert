#include "p2/thermal/temporal_filter.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace p2 {
namespace {

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr)
        *error = message;
}

bool all_finite(const std::array<float, kMlx90640Pixels> &values)
{
    for (const float value : values) {
        if (!std::isfinite(value))
            return false;
    }
    return true;
}

}  // namespace

ThermalTemporalFilter::ThermalTemporalFilter(
    ThermalTemporalFilterConfig config)
    : config_(std::move(config))
{
}

bool ThermalTemporalFilter::process(
    std::uint64_t timestamp_ns,
    const std::array<float, kMlx90640Pixels> &input,
    std::array<float, kMlx90640Pixels> *output,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (output == nullptr) {
        set_error(error, "thermal filter output pointer is null");
        return false;
    }
    if (!std::isfinite(config_.time_constant_ms) ||
        config_.time_constant_ms <= 0.0F || config_.reset_gap_ns == 0) {
        ++stats_.rejected_config;
        set_error(error, "thermal filter configuration is invalid");
        return false;
    }
    if (timestamp_ns == 0 ||
        (initialized_ && timestamp_ns <= last_timestamp_ns_)) {
        ++stats_.rejected_timestamp;
        set_error(error, "thermal filter timestamp is zero or non-monotonic");
        return false;
    }
    if (!all_finite(input)) {
        ++stats_.rejected_nonfinite;
        set_error(error, "thermal filter input contains non-finite values");
        return false;
    }

    if (!initialized_) {
        state_ = input;
        last_timestamp_ns_ = timestamp_ns;
        initialized_ = true;
        ++stats_.accepted_frames;
        ++stats_.initialization_resets;
        *output = state_;
        return true;
    }

    const std::uint64_t delta_ns = timestamp_ns - last_timestamp_ns_;
    if (delta_ns > config_.reset_gap_ns) {
        state_ = input;
        last_timestamp_ns_ = timestamp_ns;
        ++stats_.accepted_frames;
        ++stats_.gap_resets;
        *output = state_;
        return true;
    }

    const double delta_ms = static_cast<double>(delta_ns) / 1'000'000.0;
    const double alpha_double = 1.0 -
        std::exp(-delta_ms / static_cast<double>(config_.time_constant_ms));
    const float alpha = static_cast<float>(
        std::clamp(alpha_double, 0.0, 1.0));
    for (std::size_t index = 0; index < state_.size(); ++index)
        state_[index] += alpha * (input[index] - state_[index]);
    last_timestamp_ns_ = timestamp_ns;
    ++stats_.accepted_frames;
    *output = state_;
    return true;
}

void ThermalTemporalFilter::reset()
{
    state_.fill(0.0F);
    last_timestamp_ns_ = 0;
    initialized_ = false;
    ++stats_.explicit_resets;
}

}  // namespace p2
