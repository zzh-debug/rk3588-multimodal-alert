#include "p2/thermal/colormap.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace p2 {
namespace {

constexpr std::array<Rgb8, 5> kPalette{{
    {0, 0, 64},
    {0, 128, 255},
    {0, 200, 80},
    {255, 220, 0},
    {220, 0, 0},
}};

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr)
        *error = message;
}

std::uint8_t interpolate(std::uint8_t first, std::uint8_t second,
                         float fraction)
{
    const float value = static_cast<float>(first) +
        fraction * (static_cast<float>(second) -
                    static_cast<float>(first));
    return static_cast<std::uint8_t>(std::lround(
        std::clamp(value, 0.0F, 255.0F)));
}

Rgb8 palette_color(float normalized)
{
    const float scaled = normalized *
        static_cast<float>(kPalette.size() - 1);
    const std::size_t first = std::min(
        static_cast<std::size_t>(scaled), kPalette.size() - 2);
    const std::size_t second = first + 1;
    const float fraction = scaled - static_cast<float>(first);
    return {
        interpolate(kPalette[first].red, kPalette[second].red, fraction),
        interpolate(kPalette[first].green, kPalette[second].green, fraction),
        interpolate(kPalette[first].blue, kPalette[second].blue, fraction),
    };
}

}  // namespace

bool apply_thermal_colormap(
    const std::array<float, kMlx90640Pixels> &temperature_c,
    const ThermalColorMapConfig &config,
    ThermalColorMapResult *result,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (result == nullptr) {
        set_error(error, "thermal colormap result pointer is null");
        return false;
    }
    if (!std::isfinite(config.min_temperature_c) ||
        !std::isfinite(config.max_temperature_c) ||
        config.max_temperature_c <= config.min_temperature_c) {
        set_error(error, "thermal colormap temperature range is invalid");
        return false;
    }

    ThermalColorMapResult output;
    const float range =
        config.max_temperature_c - config.min_temperature_c;
    for (std::size_t index = 0; index < temperature_c.size(); ++index) {
        const float value = temperature_c[index];
        if (!std::isfinite(value)) {
            output.rgb[index] = config.invalid_color;
            ++output.invalid_pixels;
            continue;
        }
        ++output.finite_pixels;
        if (value <= config.min_temperature_c)
            ++output.clamped_low_pixels;
        if (value >= config.max_temperature_c)
            ++output.clamped_high_pixels;
        const float normalized = std::clamp(
            (value - config.min_temperature_c) / range, 0.0F, 1.0F);
        output.rgb[index] = palette_color(normalized);
    }
    *result = output;
    return true;
}

}  // namespace p2
