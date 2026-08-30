#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace p2 {

struct LumaStatistics {
    bool valid = false;
    std::size_t samples = 0;
    float mean = 0.0F;
    std::uint8_t p10 = 0;
    std::uint8_t p50 = 0;
    std::uint8_t p90 = 0;
    float dark_fraction = 0.0F;
    float bright_fraction = 0.0F;
};

bool analyze_nv12_luma(const std::uint8_t *data,
                       std::size_t size,
                       std::uint32_t width,
                       std::uint32_t height,
                       std::uint32_t stride,
                       LumaStatistics *statistics,
                       std::string *error);

struct ExposureTelemetry {
    bool valid = false;
    std::int32_t exposure = 0;
    std::int32_t exposure_min = 0;
    std::int32_t exposure_max = 0;
    std::int32_t analogue_gain = 0;
    std::int32_t analogue_gain_min = 0;
    std::int32_t analogue_gain_max = 0;
    float exposure_ratio = 0.0F;
    float analogue_gain_ratio = 0.0F;
};

enum class IlluminationState {
    off,
    dark_pending,
    ramp,
    on,
    hold,
    cooldown,
    fault,
};

enum class RampDirection {
    none,
    up,
    down,
};

const char *to_string(IlluminationState state);
const char *to_string(RampDirection direction);

struct IlluminationConfig {
    std::uint8_t dark_p50_max = 60;
    std::uint8_t dark_p90_max = 110;
    std::uint8_t ramp_stop_p50 = 82;
    std::uint8_t ramp_stop_p90 = 145;
    float minimum_exposure_ratio = 0.55F;
    float minimum_gain_ratio = 0.15F;
    std::uint32_t target_brightness = 32;
    std::uint32_t ramp_step = 4;
    std::uint64_t dark_pending_ns = 1'000'000'000ULL;
    std::uint64_t ramp_step_ns = 250'000'000ULL;
    std::uint64_t hold_ns = 2'000'000'000ULL;
    std::uint64_t cooldown_ns = 3'000'000'000ULL;
};

struct IlluminationObservation {
    std::uint64_t timestamp_ns = 0;
    LumaStatistics luma;
    ExposureTelemetry exposure;
    bool sustained_thermal_target = false;
};

struct IlluminationUpdate {
    IlluminationState state = IlluminationState::off;
    RampDirection ramp_direction = RampDirection::none;
    std::uint32_t desired_brightness = 0;
    bool dark_condition = false;
    bool trigger_condition = false;
    bool state_changed = false;
};

struct IlluminationStats {
    std::uint64_t updates = 0;
    std::uint64_t transitions = 0;
    std::uint64_t activations = 0;
    std::uint64_t deactivations = 0;
    std::uint64_t faults = 0;
    std::uint32_t maximum_brightness = 0;
};

class IlluminationController {
public:
    explicit IlluminationController(IlluminationConfig config = {});

    bool update(const IlluminationObservation &observation,
                IlluminationUpdate *update,
                std::string *error);
    void force_fault(IlluminationUpdate *update);
    void reset();

    IlluminationState state() const { return state_; }
    std::uint32_t desired_brightness() const { return brightness_; }
    const IlluminationStats &stats() const { return stats_; }

private:
    void enter_state(IlluminationState state,
                     RampDirection direction,
                     std::uint64_t timestamp_ns,
                     bool *changed);
    void fill_update(bool dark, bool trigger, bool changed,
                     IlluminationUpdate *update) const;

    IlluminationConfig config_;
    IlluminationState state_ = IlluminationState::off;
    RampDirection ramp_direction_ = RampDirection::none;
    std::uint32_t brightness_ = 0;
    std::uint64_t state_since_ns_ = 0;
    std::uint64_t last_ramp_ns_ = 0;
    std::uint64_t last_timestamp_ns_ = 0;
    bool have_timestamp_ = false;
    IlluminationStats stats_;
};

}  // namespace p2
