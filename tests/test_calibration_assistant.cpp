#include "p2/calibration/assistant.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool near(double actual, double expected, double tolerance = 1.0e-9)
{
    return std::fabs(actual - expected) <= tolerance;
}

p2::ThermalHotspot make_hotspot(double x, double y, double weight = 10.0)
{
    p2::ThermalHotspot hotspot;
    hotspot.centroid = {x, y};
    hotspot.pixel_count = 4;
    hotspot.peak_delta_c = 8.0F;
    hotspot.total_weight = weight;
    return hotspot;
}

}  // namespace

int main()
{
    std::string error;
    const p2::ImageDimensions source{3840, 2160};
    const std::array<p2::ImageDisplayTransform, 6> transforms{{
        {p2::RightAngleRotation::k0, false, false},
        {p2::RightAngleRotation::k0, true, true},
        {p2::RightAngleRotation::kClockwise90, false, false},
        {p2::RightAngleRotation::kClockwise90, true, false},
        {p2::RightAngleRotation::k180, false, true},
        {p2::RightAngleRotation::kClockwise270, true, true},
    }};
    const std::array<p2::Point2d, 4> source_points{{
        {0.0, 0.0},
        {3839.0, 2159.0},
        {1234.5, 987.25},
        {2000.0, 500.0},
    }};
    for (const p2::ImageDisplayTransform &transform : transforms) {
        for (const p2::Point2d &source_point : source_points) {
            p2::Point2d normalized;
            p2::Point2d roundtrip;
            if (!require(p2::source_to_display_normalized(
                             source, transform, source_point,
                             &normalized, &error),
                         "source point maps to display") ||
                !require(p2::display_normalized_to_source(
                             source, transform, normalized,
                             &roundtrip, &error),
                         "display point maps back to source") ||
                !require(near(roundtrip.x, source_point.x, 1.0e-8) &&
                             near(roundtrip.y, source_point.y, 1.0e-8),
                         "display transform roundtrip is exact"))
                return EXIT_FAILURE;
        }
    }

    p2::Point2d point;
    if (!require(p2::display_normalized_to_source(
                     source,
                     {p2::RightAngleRotation::kClockwise90, false, false},
                     {0.0, 0.0}, &point, &error),
                 "rotated top-left maps") ||
        !require(near(point.x, 0.0) && near(point.y, 2159.0),
                 "clockwise rotation has the expected handedness"))
        return EXIT_FAILURE;

    std::array<float, p2::kThermalColumns * p2::kThermalRows> background{};
    std::array<float, p2::kThermalColumns * p2::kThermalRows> scene{};
    background.fill(24.0F);
    scene = background;
    const auto set_delta = [&](std::size_t column, std::size_t row,
                               float delta) {
        scene[row * p2::kThermalColumns + column] += delta;
    };
    set_delta(10, 8, 8.0F);
    set_delta(11, 8, 6.0F);
    set_delta(10, 9, 4.0F);
    set_delta(11, 9, 2.0F);

    p2::ThermalHotspotConfig hotspot_config;
    hotspot_config.minimum_delta_c = 1.5F;
    hotspot_config.minimum_peak_delta_c = 5.0F;
    p2::ThermalHotspot hotspot;
    if (!require(p2::detect_thermal_hotspot(
                     scene, background, hotspot_config, &hotspot, &error),
                 "connected warm target is detected") ||
        !require(hotspot.pixel_count == 2,
                 "only the high-temperature core is retained") ||
        !require(near(hotspot.centroid.x, 10.0 + 6.0 / 14.0) &&
                     near(hotspot.centroid.y, 8.0),
                 "high-core weighted centroid is deterministic") ||
        !require(near(hotspot.peak.x, 10.0) &&
                     near(hotspot.peak.y, 8.0),
                 "hottest pixel is retained") ||
        !require(near(hotspot.total_weight, 14.0) &&
                     near(hotspot.core_threshold_delta_c, 4.8, 1.0e-6),
                 "hotspot reports core weight and adaptive threshold"))
        return EXIT_FAILURE;

    std::array<float, p2::kThermalColumns * p2::kThermalRows> empty =
        background;
    if (!require(!p2::detect_thermal_hotspot(
                     empty, background, hotspot_config, &hotspot, &error),
                 "empty scene is rejected"))
        return EXIT_FAILURE;

    set_delta(25, 18, 7.0F);
    set_delta(26, 18, 7.0F);
    hotspot_config.maximum_secondary_peak_ratio = 0.6;
    if (!require(!p2::detect_thermal_hotspot(
                     scene, background, hotspot_config, &hotspot, &error),
                 "similarly strong second heat source is rejected"))
        return EXIT_FAILURE;

    scene = background;
    for (std::size_t row = 12; row < 18; ++row) {
        for (std::size_t column = 20; column < 28; ++column)
            set_delta(column, row, 5.5F);
    }
    set_delta(5, 5, 14.0F);
    set_delta(6, 5, 12.0F);
    set_delta(5, 6, 10.0F);
    set_delta(6, 6, 8.0F);
    hotspot_config.maximum_secondary_peak_ratio = 0.85;
    if (!require(p2::detect_thermal_hotspot(
                     scene, background, hotspot_config, &hotspot, &error),
                 "compact hotter target beats a large mild body region") ||
        !require(hotspot.pixel_count == 3 &&
                     hotspot.centroid.x < 6.0 && hotspot.centroid.y < 6.0,
                 "selected centroid stays on the compact hot core") ||
        !require(near(hotspot.peak.x, 5.0) &&
                     near(hotspot.peak.y, 5.0),
                 "compact target owns the selected peak"))
        return EXIT_FAILURE;

    scene = background;
    set_delta(8, 6, 14.0F);
    set_delta(9, 6, 6.0F);
    if (!require(!p2::detect_thermal_hotspot(
                     scene, background, hotspot_config, &hotspot, &error),
                 "one-pixel distant target is rejected") ||
        !require(error.find("距离过远") != std::string::npos,
                 "distant-target rejection explains the physical limit"))
        return EXIT_FAILURE;

    scene = background;
    set_delta(4, 10, 20.0F);
    for (std::size_t column = 5; column <= 12; ++column)
        set_delta(column, 10, 12.0F);
    if (!require(!p2::detect_thermal_hotspot(
                     scene, background, hotspot_config, &hotspot, &error),
                 "centroid pulled far from the hottest core is rejected"))
        return EXIT_FAILURE;

    const std::vector<p2::ThermalHotspot> stable_samples{
        make_hotspot(10.0, 8.0, 10.0),
        make_hotspot(10.1, 8.1, 12.0),
        make_hotspot(9.9, 7.9, 11.0),
        make_hotspot(10.0, 8.0, 9.0),
    };
    p2::StableHotspot stable;
    if (!require(p2::summarize_stable_hotspots(
                     stable_samples, 0.25, &stable, &error),
                 "stable thermal samples are accepted") ||
        !require(stable.sample_count == stable_samples.size(),
                 "stable sample count is reported") ||
        !require(stable.maximum_deviation_pixels < 0.2,
                 "stable sample deviation is measured"))
        return EXIT_FAILURE;

    std::vector<p2::ThermalHotspot> moving = stable_samples;
    moving.push_back(make_hotspot(12.0, 8.0, 10.0));
    if (!require(!p2::summarize_stable_hotspots(
                     moving, 0.5, &stable, &error),
                 "moving target is rejected"))
        return EXIT_FAILURE;

    const std::array<double, 9> homography{
        100.0, 0.0, 50.0,
        0.0, 80.0, 40.0,
        0.0, 0.0, 1.0,
    };
    const std::vector<p2::CrossSpectralCorrespondence> validation{
        {{0.0, 0.0}, {50.0, 40.0}},
        {{10.0, 5.0}, {1053.0, 438.0}},
        {{20.0, 10.0}, {2044.0, 846.0}},
        {{30.0, 20.0}, {3050.0, 1640.0}},
    };
    p2::MappingErrorDistribution distribution;
    if (!require(p2::calculate_mapping_errors(
                     homography, validation, &distribution, &error),
                 "validation error distribution is calculated") ||
        !require(distribution.errors_px.size() == validation.size(),
                 "all validation errors are retained") ||
        !require(
            near(distribution.errors_px.front(), 0.0) &&
                near(distribution.errors_px.back(), std::sqrt(72.0)),
                 "validation errors are sorted") ||
        !require(distribution.p95_px > distribution.p50_px &&
                     near(distribution.maximum_px, std::sqrt(72.0)),
                 "validation percentiles and maximum are reported"))
        return EXIT_FAILURE;

    std::cout << "calibration assistant tests passed\n";
    return EXIT_SUCCESS;
}
