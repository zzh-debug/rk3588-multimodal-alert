#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "p2/calibration/cross_spectral.hpp"

namespace p2 {

enum class RightAngleRotation {
    k0,
    kClockwise90,
    k180,
    kClockwise270,
};

struct ImageDimensions {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ImageDisplayTransform {
    RightAngleRotation rotation = RightAngleRotation::k0;
    bool flip_horizontal = false;
    bool flip_vertical = false;
};

bool transformed_dimensions(
    ImageDimensions source,
    const ImageDisplayTransform &transform,
    ImageDimensions *display,
    std::string *error);

bool source_to_display_normalized(
    ImageDimensions source,
    const ImageDisplayTransform &transform,
    const Point2d &source_point,
    Point2d *display_normalized,
    std::string *error);

bool display_normalized_to_source(
    ImageDimensions source,
    const ImageDisplayTransform &transform,
    const Point2d &display_normalized,
    Point2d *source_point,
    std::string *error);

struct ThermalHotspotConfig {
    float minimum_delta_c = 2.5F;
    float minimum_peak_delta_c = 6.0F;
    float centroid_peak_fraction = 0.60F;
    std::size_t minimum_pixels = 2;
    std::size_t maximum_pixels = 96;
    double maximum_secondary_peak_ratio = 0.85;
    double maximum_centroid_to_peak_distance_pixels = 2.0;
};

struct ThermalHotspot {
    Point2d centroid;
    Point2d peak;
    Rect2d pixel_center_bounds;
    std::size_t pixel_count = 0;
    float peak_delta_c = 0.0F;
    float core_threshold_delta_c = 0.0F;
    double total_weight = 0.0;
    double secondary_peak_ratio = 0.0;
    double centroid_to_peak_distance_pixels = 0.0;
};

bool detect_thermal_hotspot(
    const std::array<float, kThermalColumns * kThermalRows> &temperature_c,
    const std::array<float, kThermalColumns * kThermalRows> &background_c,
    const ThermalHotspotConfig &config,
    ThermalHotspot *hotspot,
    std::string *error);

struct StableHotspot {
    Point2d centroid;
    double maximum_deviation_pixels = 0.0;
    double mean_peak_delta_c = 0.0;
    std::size_t sample_count = 0;
};

bool summarize_stable_hotspots(
    const std::vector<ThermalHotspot> &samples,
    double maximum_allowed_deviation_pixels,
    StableHotspot *summary,
    std::string *error);

struct MappingErrorDistribution {
    double root_mean_square_px = 0.0;
    double p50_px = 0.0;
    double p95_px = 0.0;
    double maximum_px = 0.0;
    std::vector<double> errors_px;
};

bool calculate_mapping_errors(
    const std::array<double, 9> &thermal_to_visible,
    const std::vector<CrossSpectralCorrespondence> &validation_points,
    MappingErrorDistribution *distribution,
    std::string *error);

}  // namespace p2
