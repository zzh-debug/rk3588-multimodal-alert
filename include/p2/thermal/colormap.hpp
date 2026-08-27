#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "p2/thermal/mlx90640_math.hpp"

namespace p2 {

struct Rgb8 {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    bool operator==(const Rgb8 &other) const
    {
        return red == other.red && green == other.green &&
            blue == other.blue;
    }
};

struct ThermalColorMapConfig {
    float min_temperature_c = 20.0F;
    float max_temperature_c = 45.0F;
    Rgb8 invalid_color{255, 0, 255};
};

struct ThermalColorMapResult {
    std::array<Rgb8, kMlx90640Pixels> rgb{};
    std::size_t finite_pixels = 0;
    std::size_t invalid_pixels = 0;
    std::size_t clamped_low_pixels = 0;
    std::size_t clamped_high_pixels = 0;
};

bool apply_thermal_colormap(
    const std::array<float, kMlx90640Pixels> &temperature_c,
    const ThermalColorMapConfig &config,
    ThermalColorMapResult *result,
    std::string *error);

}  // namespace p2
