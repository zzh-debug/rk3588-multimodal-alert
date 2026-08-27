#include "p2/thermal/colormap.hpp"
#include "p2/thermal/temporal_filter.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool near(float actual, float expected, float tolerance = 0.001F)
{
    return std::fabs(actual - expected) <= tolerance;
}

}  // namespace

int main()
{
    p2::ThermalTemporalFilterConfig filter_config;
    filter_config.time_constant_ms = 1000.0F;
    filter_config.reset_gap_ns = 5'000'000'000ULL;
    p2::ThermalTemporalFilter filter(filter_config);
    std::array<float, p2::kMlx90640Pixels> input{};
    std::array<float, p2::kMlx90640Pixels> output{};
    std::string error;

    input.fill(0.0F);
    if (!require(filter.process(1'000'000'000ULL, input, &output, &error),
                 "filter accepts first frame") ||
        !require(output == input, "first frame initializes without lag"))
        return EXIT_FAILURE;

    input.fill(100.0F);
    if (!require(filter.process(2'000'000'000ULL, input, &output, &error),
                 "filter accepts second frame") ||
        !require(near(output[0], 63.212055F),
                 "time-aware EMA matches 1-exp(-dt/tau)"))
        return EXIT_FAILURE;

    const auto state_before_rejection = output;
    input[17] = std::numeric_limits<float>::quiet_NaN();
    if (!require(!filter.process(3'000'000'000ULL, input, &output, &error),
                 "non-finite frame rejected") ||
        !require(filter.last_timestamp_ns() == 2'000'000'000ULL,
                 "rejected frame does not advance timestamp"))
        return EXIT_FAILURE;
    input[17] = 100.0F;
    if (!require(!filter.process(2'000'000'000ULL, input, &output, &error),
                 "non-monotonic timestamp rejected"))
        return EXIT_FAILURE;

    input.fill(25.0F);
    if (!require(filter.process(8'000'000'001ULL, input, &output, &error),
                 "long-gap frame accepted") ||
        !require(output == input, "long gap resets stale state") ||
        !require(filter.stats().gap_resets == 1,
                 "long-gap reset counted") ||
        !require(filter.stats().rejected_nonfinite == 1,
                 "non-finite rejection counted") ||
        !require(filter.stats().rejected_timestamp == 1,
                 "timestamp rejection counted") ||
        !require(state_before_rejection[0] > 0.0F,
                 "pre-rejection state was meaningful"))
        return EXIT_FAILURE;

    filter.reset();
    if (!require(!filter.initialized(), "explicit reset clears state") ||
        !require(filter.stats().explicit_resets == 1,
                 "explicit reset counted"))
        return EXIT_FAILURE;

    p2::ThermalColorMapConfig color_config;
    color_config.min_temperature_c = 0.0F;
    color_config.max_temperature_c = 100.0F;
    std::array<float, p2::kMlx90640Pixels> temperatures{};
    temperatures.fill(50.0F);
    temperatures[0] = -10.0F;
    temperatures[1] = 110.0F;
    temperatures[2] = std::numeric_limits<float>::quiet_NaN();
    p2::ThermalColorMapResult colors;
    if (!require(p2::apply_thermal_colormap(
                     temperatures, color_config, &colors, &error),
                 "valid colormap accepted") ||
        !require(colors.rgb[0] == p2::Rgb8{0, 0, 64},
                 "cold endpoint clamped") ||
        !require(colors.rgb[1] == p2::Rgb8{220, 0, 0},
                 "hot endpoint clamped") ||
        !require(colors.rgb[2] == color_config.invalid_color,
                 "invalid temperature uses diagnostic color") ||
        !require(colors.rgb[3] == p2::Rgb8{0, 200, 80},
                 "midpoint uses center palette stop") ||
        !require(colors.invalid_pixels == 1 &&
                     colors.clamped_low_pixels == 1 &&
                     colors.clamped_high_pixels == 1,
                 "colormap counters classified"))
        return EXIT_FAILURE;

    color_config.max_temperature_c = color_config.min_temperature_c;
    if (!require(!p2::apply_thermal_colormap(
                     temperatures, color_config, &colors, &error),
                 "invalid colormap range rejected"))
        return EXIT_FAILURE;

    std::cout << "thermal processing tests passed\n";
    return EXIT_SUCCESS;
}
