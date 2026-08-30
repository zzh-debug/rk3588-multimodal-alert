#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "p2/capture/frame_types.hpp"

namespace p2 {

inline constexpr std::size_t kMlx90640Width = 32;
inline constexpr std::size_t kMlx90640Height = 24;
inline constexpr std::size_t kMlx90640Pixels =
    kMlx90640Width * kMlx90640Height;
inline constexpr std::size_t kMlx90640EepromBytes = 1664;

struct ThermalMathConfig {
    float emissivity = 0.95F;
    float reflected_temperature_offset_c = -8.0F;
    bool correct_bad_pixels = true;
    std::size_t max_repairable_nonfinite_pixels = 4;
};

struct ThermalMathResult {
    std::array<float, kMlx90640Pixels> temperature_c{};
    std::array<float, kMlx90640Pixels> image{};
    std::array<float, 2> subpage_ta_c{};
    std::array<float, 2> subpage_vdd{};
    std::array<std::uint16_t, 2> subpage_id{};
    float ta_c = 0.0F;
    float vdd = 0.0F;
    float min_temperature_c = 0.0F;
    float max_temperature_c = 0.0F;
    std::uint32_t pair_sequence = 0;
    std::uint64_t subpage_span_ns = 0;
    std::uint64_t calculation_time_ns = 0;
    std::size_t finite_temperature_pixels = 0;
    std::size_t finite_image_pixels = 0;
    std::size_t repaired_nonfinite_temperature_pixels = 0;
    std::size_t repaired_nonfinite_image_pixels = 0;
    std::size_t broken_pixel_count = 0;
    std::size_t outlier_pixel_count = 0;
    std::array<std::uint16_t, 5> broken_pixels{};
    std::array<std::uint16_t, 5> outlier_pixels{};
};

bool repair_nonfinite_thermal_pixels(
    std::array<float, kMlx90640Pixels> *pixels,
    std::size_t maximum_repairable_pixels,
    std::size_t *repaired_pixels,
    std::string *error);

class Mlx90640Math {
public:
    Mlx90640Math();
    ~Mlx90640Math();

    Mlx90640Math(const Mlx90640Math &) = delete;
    Mlx90640Math &operator=(const Mlx90640Math &) = delete;
    Mlx90640Math(Mlx90640Math &&) noexcept;
    Mlx90640Math &operator=(Mlx90640Math &&) noexcept;

    bool initialize_eeprom_be(const std::uint8_t *bytes, std::size_t size,
                              std::string *error);
    bool calculate(const ThermalFramePayload &frame,
                   const ThermalMathConfig &config,
                   ThermalMathResult *result, std::string *error) const;
    bool initialized() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace p2
