#include "p2/illumination/illumination_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

p2::IlluminationObservation observation(std::uint64_t timestamp,
                                        bool thermal,
                                        std::uint8_t p50 = 20,
                                        std::uint8_t p90 = 30)
{
    p2::IlluminationObservation value;
    value.timestamp_ns = timestamp;
    value.luma.valid = true;
    value.luma.samples = 100;
    value.luma.mean = static_cast<float>(p50);
    value.luma.p10 = p50;
    value.luma.p50 = p50;
    value.luma.p90 = p90;
    value.exposure.valid = true;
    value.exposure.exposure_ratio = 0.8F;
    value.exposure.analogue_gain_ratio = 0.4F;
    value.sustained_thermal_target = thermal;
    return value;
}

}  // namespace

int main()
{
    std::vector<std::uint8_t> nv12(128U * 64U * 3U / 2U, 128U);
    for (std::size_t y = 0; y < 64U; ++y) {
        for (std::size_t x = 0; x < 128U; ++x)
            nv12[y * 128U + x] = x < 64U ? 20U : 100U;
    }
    p2::LumaStatistics luma;
    std::string error;
    if (!require(p2::analyze_nv12_luma(
                     nv12.data(), nv12.size(), 128, 64, 128,
                     &luma, &error),
                 "NV12 luma analysis succeeds") ||
        !require(luma.valid && luma.samples > 2000U,
                 "luma analysis samples bounded ROI") ||
        !require(luma.p10 == 20U && luma.p50 == 20U &&
                     luma.p90 == 100U,
                 "luma percentiles are deterministic") ||
        !require(std::fabs(luma.mean - 60.0F) < 2.0F,
                 "luma mean is correct") ||
        !require(!p2::analyze_nv12_luma(
                     nv12.data(), 16, 128, 64, 128, &luma, &error),
                 "short NV12 frame is rejected"))
        return EXIT_FAILURE;

    p2::IlluminationConfig config;
    config.target_brightness = 12;
    config.ramp_step = 4;
    config.dark_pending_ns = 100;
    config.ramp_step_ns = 50;
    config.hold_ns = 100;
    config.cooldown_ns = 100;
    p2::IlluminationController controller(config);
    p2::IlluminationUpdate update;
    if (!require(controller.update(observation(1, true), &update, &error) &&
                     update.state == p2::IlluminationState::dark_pending &&
                     update.desired_brightness == 0U,
                 "dark thermal target enters pending") ||
        !require(controller.update(observation(50, true), &update, &error) &&
                     update.state == p2::IlluminationState::dark_pending,
                 "pending interval is enforced") ||
        !require(controller.update(observation(101, true), &update, &error) &&
                     update.state == p2::IlluminationState::ramp &&
                     update.ramp_direction == p2::RampDirection::up &&
                     update.desired_brightness == 4U,
                 "confirmed target starts gradual ramp") ||
        !require(controller.update(observation(151, true), &update, &error) &&
                     update.desired_brightness == 8U,
                 "ramp advances one bounded step") ||
        !require(controller.update(observation(201, true), &update, &error) &&
                     update.state == p2::IlluminationState::on &&
                     update.desired_brightness == 12U,
                 "ramp reaches configured target") ||
        !require(controller.update(observation(210, false), &update, &error) &&
                     update.state == p2::IlluminationState::hold &&
                     update.desired_brightness == 12U,
                 "target loss enters hold") ||
        !require(controller.update(observation(250, true), &update, &error) &&
                     update.state == p2::IlluminationState::on &&
                     update.desired_brightness == 12U,
                 "target return cancels hold") ||
        !require(controller.update(observation(300, false), &update, &error) &&
                     update.state == p2::IlluminationState::hold,
                 "second target loss enters hold") ||
        !require(controller.update(observation(400, false), &update, &error) &&
                     update.state == p2::IlluminationState::ramp &&
                     update.ramp_direction == p2::RampDirection::down &&
                     update.desired_brightness == 8U,
                 "hold expiry starts gradual release") ||
        !require(controller.update(observation(450, false), &update, &error) &&
                     update.desired_brightness == 4U,
                 "release ramp advances") ||
        !require(controller.update(observation(500, false), &update, &error) &&
                     update.state == p2::IlluminationState::cooldown &&
                     update.desired_brightness == 0U,
                 "release reaches off and enters cooldown") ||
        !require(controller.update(observation(550, true), &update, &error) &&
                     update.state == p2::IlluminationState::cooldown,
                 "cooldown blocks immediate reactivation") ||
        !require(controller.update(observation(600, true), &update, &error) &&
                     update.state == p2::IlluminationState::dark_pending,
                 "persistent target restarts after cooldown"))
        return EXIT_FAILURE;

    if (!require(!controller.update(observation(600, true), &update, &error) &&
                     update.state == p2::IlluminationState::fault &&
                     update.desired_brightness == 0U,
                 "non-monotonic telemetry faults safely") ||
        !require(controller.stats().activations == 1U &&
                     controller.stats().deactivations == 1U &&
                     controller.stats().faults == 1U &&
                     controller.stats().maximum_brightness == 12U,
                 "state machine statistics are auditable"))
        return EXIT_FAILURE;

    controller.shutdown(&update);
    if (!require(update.state == p2::IlluminationState::off &&
                     update.desired_brightness == 0U &&
                     controller.state() == p2::IlluminationState::off,
                 "shutdown returns controller to OFF"))
        return EXIT_FAILURE;

    controller.reset();
    if (!require(controller.update(observation(1, true), &update, &error),
                 "adaptive ramp pending starts") ||
        !require(controller.update(observation(101, true), &update, &error) &&
                     update.desired_brightness == 4U,
                 "adaptive ramp begins") ||
        !require(controller.update(observation(120, true, 90, 150),
                                   &update, &error) &&
                     update.state == p2::IlluminationState::on &&
                     update.desired_brightness == 4U,
                 "sufficient luma stops ramp below safety target"))
        return EXIT_FAILURE;

    bool threw = false;
    try {
        p2::IlluminationConfig invalid = config;
        invalid.ramp_step = 0;
        p2::IlluminationController rejected(invalid);
        (void)rejected;
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    if (!require(threw, "invalid illumination configuration is rejected"))
        return EXIT_FAILURE;

    std::cout << "illumination controller tests passed\n";
    return EXIT_SUCCESS;
}
