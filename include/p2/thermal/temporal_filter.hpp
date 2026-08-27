#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "p2/thermal/mlx90640_math.hpp"

namespace p2 {

struct ThermalTemporalFilterConfig {
    float time_constant_ms = 250.0F;
    std::uint64_t reset_gap_ns = 1'000'000'000ULL;
};

struct ThermalTemporalFilterStats {
    std::uint64_t accepted_frames = 0;
    std::uint64_t initialization_resets = 0;
    std::uint64_t gap_resets = 0;
    std::uint64_t explicit_resets = 0;
    std::uint64_t rejected_nonfinite = 0;
    std::uint64_t rejected_timestamp = 0;
    std::uint64_t rejected_config = 0;
};

class ThermalTemporalFilter {
public:
    explicit ThermalTemporalFilter(
        ThermalTemporalFilterConfig config = {});

    bool process(
        std::uint64_t timestamp_ns,
        const std::array<float, kMlx90640Pixels> &input,
        std::array<float, kMlx90640Pixels> *output,
        std::string *error);
    void reset();

    const ThermalTemporalFilterConfig &config() const { return config_; }
    const ThermalTemporalFilterStats &stats() const { return stats_; }
    bool initialized() const { return initialized_; }
    std::uint64_t last_timestamp_ns() const { return last_timestamp_ns_; }

private:
    ThermalTemporalFilterConfig config_;
    ThermalTemporalFilterStats stats_;
    std::array<float, kMlx90640Pixels> state_{};
    std::uint64_t last_timestamp_ns_ = 0;
    bool initialized_ = false;
};

}  // namespace p2
