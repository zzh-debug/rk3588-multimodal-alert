#include "p2/thermal/mlx90640_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

bool read_file(const char *path, std::vector<std::uint8_t> *bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes->assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool require(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

}  // namespace

int main(int argc, char **argv)
{
    p2::Mlx90640Math validation_math;
    std::string error;
    const std::uint8_t invalid_eeprom = 0;
    if (!require(!validation_math.initialize_eeprom_be(
                     &invalid_eeprom, 1, &error),
                 "invalid EEPROM size must be rejected"))
        return EXIT_FAILURE;

    if (argc == 1) {
        std::cout << "validation-only test passed\n";
        return EXIT_SUCCESS;
    }
    if (argc != 3) {
        std::cerr << "usage: test_mlx90640_math EEPROM ZMLX\n";
        return EXIT_FAILURE;
    }

    std::vector<std::uint8_t> eeprom;
    std::vector<std::uint8_t> zmlx;
    if (!require(read_file(argv[1], &eeprom), "read EEPROM") ||
        !require(read_file(argv[2], &zmlx), "read ZMLX") ||
        !require(zmlx.size() == p2::kZmlxMetaV1Bytes,
                 "ZMLX byte count"))
        return EXIT_FAILURE;

    p2::Mlx90640Math math;
    if (!require(math.initialize_eeprom_be(eeprom.data(), eeprom.size(),
                                           &error),
                 error.c_str()))
        return EXIT_FAILURE;

    p2::ThermalFramePayload frame;
    std::copy(zmlx.begin(), zmlx.end(), frame.zmlx_bytes.begin());
    p2::ThermalMathConfig config;
    p2::ThermalMathResult first;
    p2::ThermalMathResult second;
    if (!math.calculate(frame, config, &first, &error)) {
        std::cerr << "FAIL: " << error << '\n';
        return EXIT_FAILURE;
    }
    if (!math.calculate(frame, config, &second, &error)) {
        std::cerr << "FAIL: " << error << '\n';
        return EXIT_FAILURE;
    }

    p2::ThermalMathConfig uncorrected_config = config;
    uncorrected_config.correct_bad_pixels = false;
    p2::ThermalMathResult uncorrected;
    if (!math.calculate(frame, uncorrected_config, &uncorrected, &error)) {
        std::cerr << "FAIL: " << error << '\n';
        return EXIT_FAILURE;
    }
    std::size_t corrected_pixel_differences = 0;
    for (std::size_t index = 0; index < p2::kMlx90640Pixels; ++index) {
        if (first.temperature_c[index] != uncorrected.temperature_c[index])
            ++corrected_pixel_differences;
    }

    if (!require(first.finite_temperature_pixels == p2::kMlx90640Pixels,
                 "all temperatures finite") ||
        !require(first.finite_image_pixels == p2::kMlx90640Pixels,
                 "all image values finite") ||
        !require(first.subpage_id[0] != first.subpage_id[1],
                 "different subpage ids") ||
        !require(first.subpage_span_ns > 0, "positive subpage span") ||
        !require(first.temperature_c == second.temperature_c,
                 "temperature calculation deterministic") ||
        !require(first.image == second.image,
                 "image calculation deterministic") ||
        !require(corrected_pixel_differences > 0,
                 "bad-pixel correction changes declared pixels") ||
        !require(corrected_pixel_differences <=
                     first.broken_pixel_count + first.outlier_pixel_count,
                 "bad-pixel correction stays within declared pixel set") ||
        !require(std::isfinite(first.ta_c) && std::isfinite(first.vdd),
                 "finite scalar outputs"))
        return EXIT_FAILURE;

    p2::ThermalMathConfig invalid_config;
    invalid_config.emissivity = 0.0F;
    if (!require(!math.calculate(frame, invalid_config, &second, &error),
                 "zero emissivity rejected"))
        return EXIT_FAILURE;

    p2::ThermalFramePayload bad_header = frame;
    bad_header.zmlx_bytes[0] ^= 0xFFU;
    if (!require(!math.calculate(bad_header, config, &second, &error),
                 "bad ZMLX header rejected"))
        return EXIT_FAILURE;

    std::cout << "golden thermal math passed: ta=" << first.ta_c
              << " vdd=" << first.vdd
              << " min=" << first.min_temperature_c
              << " max=" << first.max_temperature_c
              << " broken=" << first.broken_pixel_count
              << " outlier=" << first.outlier_pixel_count << '\n';
    return EXIT_SUCCESS;
}
