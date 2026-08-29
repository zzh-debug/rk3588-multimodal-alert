#include "p2/calibration/assistant.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>

namespace p2 {
namespace {

constexpr double kCoordinateTolerance = 1.0e-9;
constexpr double kProjectiveDenominatorEpsilon = 1.0e-12;

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr)
        *error = message;
}

bool valid_dimensions(ImageDimensions dimensions)
{
    return dimensions.width >= 2 && dimensions.height >= 2;
}

bool finite_point(const Point2d &point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool in_closed_unit_interval(double value)
{
    return value >= -kCoordinateTolerance &&
        value <= 1.0 + kCoordinateTolerance;
}

Point2d rotate_source_point(
    ImageDimensions source,
    RightAngleRotation rotation,
    const Point2d &point)
{
    switch (rotation) {
    case RightAngleRotation::k0:
        return point;
    case RightAngleRotation::kClockwise90:
        return {static_cast<double>(source.height - 1) - point.y, point.x};
    case RightAngleRotation::k180:
        return {static_cast<double>(source.width - 1) - point.x,
                static_cast<double>(source.height - 1) - point.y};
    case RightAngleRotation::kClockwise270:
        return {point.y,
                static_cast<double>(source.width - 1) - point.x};
    }
    return point;
}

Point2d inverse_rotate_point(
    ImageDimensions source,
    RightAngleRotation rotation,
    const Point2d &point)
{
    switch (rotation) {
    case RightAngleRotation::k0:
        return point;
    case RightAngleRotation::kClockwise90:
        return {point.y,
                static_cast<double>(source.height - 1) - point.x};
    case RightAngleRotation::k180:
        return {static_cast<double>(source.width - 1) - point.x,
                static_cast<double>(source.height - 1) - point.y};
    case RightAngleRotation::kClockwise270:
        return {static_cast<double>(source.width - 1) - point.y, point.x};
    }
    return point;
}

struct Component {
    Point2d centroid;
    Point2d peak;
    Rect2d bounds;
    std::size_t pixels = 0;
    float peak_delta_c = -std::numeric_limits<float>::infinity();
    double weight = 0.0;
    double peak_x_sum = 0.0;
    double peak_y_sum = 0.0;
    std::size_t peak_pixels = 0;
};

double percentile(const std::vector<double> &sorted, double fraction)
{
    if (sorted.size() == 1)
        return sorted.front();
    const double position =
        fraction * static_cast<double>(sorted.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(position);
    const std::size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double interpolation = position - static_cast<double>(lower);
    return sorted[lower] +
        interpolation * (sorted[upper] - sorted[lower]);
}

bool project_point(const std::array<double, 9> &matrix,
                   const Point2d &source, Point2d *destination)
{
    const double denominator = matrix[6] * source.x +
        matrix[7] * source.y + matrix[8];
    if (!std::isfinite(denominator) ||
        std::fabs(denominator) <= kProjectiveDenominatorEpsilon)
        return false;
    destination->x =
        (matrix[0] * source.x + matrix[1] * source.y + matrix[2]) /
        denominator;
    destination->y =
        (matrix[3] * source.x + matrix[4] * source.y + matrix[5]) /
        denominator;
    return finite_point(*destination);
}

}  // namespace

bool transformed_dimensions(
    ImageDimensions source,
    const ImageDisplayTransform &transform,
    ImageDimensions *display,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (display == nullptr) {
        set_error(error, "display dimensions output pointer is null");
        return false;
    }
    if (!valid_dimensions(source)) {
        set_error(error, "source dimensions must be at least 2x2");
        return false;
    }
    if (transform.rotation == RightAngleRotation::kClockwise90 ||
        transform.rotation == RightAngleRotation::kClockwise270) {
        *display = {source.height, source.width};
    } else {
        *display = source;
    }
    return true;
}

bool source_to_display_normalized(
    ImageDimensions source,
    const ImageDisplayTransform &transform,
    const Point2d &source_point,
    Point2d *display_normalized,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (display_normalized == nullptr) {
        set_error(error, "display point output pointer is null");
        return false;
    }
    if (!valid_dimensions(source) || !finite_point(source_point) ||
        source_point.x < -kCoordinateTolerance ||
        source_point.y < -kCoordinateTolerance ||
        source_point.x > static_cast<double>(source.width - 1) +
            kCoordinateTolerance ||
        source_point.y > static_cast<double>(source.height - 1) +
            kCoordinateTolerance) {
        set_error(error, "source point is outside valid pixel centers");
        return false;
    }
    ImageDimensions display;
    if (!transformed_dimensions(source, transform, &display, error))
        return false;
    Point2d transformed =
        rotate_source_point(source, transform.rotation, source_point);
    if (transform.flip_horizontal)
        transformed.x =
            static_cast<double>(display.width - 1) - transformed.x;
    if (transform.flip_vertical)
        transformed.y =
            static_cast<double>(display.height - 1) - transformed.y;
    display_normalized->x =
        transformed.x / static_cast<double>(display.width - 1);
    display_normalized->y =
        transformed.y / static_cast<double>(display.height - 1);
    return true;
}

bool display_normalized_to_source(
    ImageDimensions source,
    const ImageDisplayTransform &transform,
    const Point2d &display_normalized,
    Point2d *source_point,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (source_point == nullptr) {
        set_error(error, "source point output pointer is null");
        return false;
    }
    if (!valid_dimensions(source) || !finite_point(display_normalized) ||
        !in_closed_unit_interval(display_normalized.x) ||
        !in_closed_unit_interval(display_normalized.y)) {
        set_error(error, "display point must be finite and normalized");
        return false;
    }
    ImageDimensions display;
    if (!transformed_dimensions(source, transform, &display, error))
        return false;
    Point2d transformed{
        std::clamp(display_normalized.x, 0.0, 1.0) *
            static_cast<double>(display.width - 1),
        std::clamp(display_normalized.y, 0.0, 1.0) *
            static_cast<double>(display.height - 1),
    };
    if (transform.flip_horizontal)
        transformed.x =
            static_cast<double>(display.width - 1) - transformed.x;
    if (transform.flip_vertical)
        transformed.y =
            static_cast<double>(display.height - 1) - transformed.y;
    *source_point =
        inverse_rotate_point(source, transform.rotation, transformed);
    return finite_point(*source_point);
}

bool detect_thermal_hotspot(
    const std::array<float, kThermalColumns * kThermalRows> &temperature_c,
    const std::array<float, kThermalColumns * kThermalRows> &background_c,
    const ThermalHotspotConfig &config,
    ThermalHotspot *hotspot,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (hotspot == nullptr) {
        set_error(error, "thermal hotspot output pointer is null");
        return false;
    }
    if (!std::isfinite(config.minimum_delta_c) ||
        !std::isfinite(config.minimum_peak_delta_c) ||
        config.minimum_delta_c <= 0.0F ||
        config.minimum_peak_delta_c < config.minimum_delta_c ||
        !std::isfinite(config.centroid_peak_fraction) ||
        config.centroid_peak_fraction <= 0.0F ||
        config.centroid_peak_fraction > 1.0F ||
        config.minimum_pixels == 0 ||
        config.maximum_pixels < config.minimum_pixels ||
        !std::isfinite(config.maximum_secondary_peak_ratio) ||
        config.maximum_secondary_peak_ratio < 0.0 ||
        config.maximum_secondary_peak_ratio > 1.0 ||
        !std::isfinite(
            config.maximum_centroid_to_peak_distance_pixels) ||
        config.maximum_centroid_to_peak_distance_pixels < 0.0) {
        set_error(error, "thermal hotspot configuration is invalid");
        return false;
    }

    constexpr std::size_t kPixels = kThermalColumns * kThermalRows;
    std::array<float, kPixels> delta{};
    std::array<bool, kPixels> foreground{};
    float global_peak_delta = -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < kPixels; ++index) {
        if (!std::isfinite(temperature_c[index]) ||
            !std::isfinite(background_c[index])) {
            set_error(error, "thermal hotspot input contains non-finite values");
            return false;
        }
        delta[index] = temperature_c[index] - background_c[index];
        global_peak_delta = std::max(global_peak_delta, delta[index]);
    }
    if (global_peak_delta < config.minimum_peak_delta_c) {
        set_error(error, "最热点温差不足，请使用更明显的暖目标");
        return false;
    }
    const float core_threshold = std::max(
        config.minimum_delta_c,
        global_peak_delta * config.centroid_peak_fraction);
    for (std::size_t index = 0; index < kPixels; ++index) {
        foreground[index] = delta[index] >= core_threshold;
    }

    std::array<bool, kPixels> visited{};
    std::vector<Component> components;
    for (std::size_t seed = 0; seed < kPixels; ++seed) {
        if (!foreground[seed] || visited[seed])
            continue;
        std::queue<std::size_t> pending;
        pending.push(seed);
        visited[seed] = true;
        Component component;
        component.bounds = {
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
        };
        double weighted_x = 0.0;
        double weighted_y = 0.0;
        while (!pending.empty()) {
            const std::size_t index = pending.front();
            pending.pop();
            const std::size_t row = index / kThermalColumns;
            const std::size_t column = index % kThermalColumns;
            const double weight = static_cast<double>(delta[index]);
            component.weight += weight;
            weighted_x += weight * static_cast<double>(column);
            weighted_y += weight * static_cast<double>(row);
            if (delta[index] > component.peak_delta_c + 1.0e-6F) {
                component.peak_delta_c = delta[index];
                component.peak_x_sum = static_cast<double>(column);
                component.peak_y_sum = static_cast<double>(row);
                component.peak_pixels = 1;
            } else if (std::fabs(
                           delta[index] - component.peak_delta_c) <=
                       1.0e-6F) {
                component.peak_x_sum += static_cast<double>(column);
                component.peak_y_sum += static_cast<double>(row);
                ++component.peak_pixels;
            }
            component.bounds.x_min =
                std::min(component.bounds.x_min,
                         static_cast<double>(column));
            component.bounds.x_max =
                std::max(component.bounds.x_max,
                         static_cast<double>(column));
            component.bounds.y_min =
                std::min(component.bounds.y_min,
                         static_cast<double>(row));
            component.bounds.y_max =
                std::max(component.bounds.y_max,
                         static_cast<double>(row));
            ++component.pixels;

            const std::array<std::pair<int, int>, 4> neighbors{{
                {-1, 0}, {1, 0}, {0, -1}, {0, 1},
            }};
            for (const auto &offset : neighbors) {
                const int next_column =
                    static_cast<int>(column) + offset.first;
                const int next_row = static_cast<int>(row) + offset.second;
                if (next_column < 0 ||
                    next_column >= static_cast<int>(kThermalColumns) ||
                    next_row < 0 ||
                    next_row >= static_cast<int>(kThermalRows))
                    continue;
                const std::size_t next =
                    static_cast<std::size_t>(next_row) * kThermalColumns +
                    static_cast<std::size_t>(next_column);
                if (foreground[next] && !visited[next]) {
                    visited[next] = true;
                    pending.push(next);
                }
            }
        }
        component.centroid = {
            weighted_x / component.weight,
            weighted_y / component.weight,
        };
        component.peak = {
            component.peak_x_sum /
                static_cast<double>(component.peak_pixels),
            component.peak_y_sum /
                static_cast<double>(component.peak_pixels),
        };
        components.push_back(component);
    }
    if (components.empty()) {
        set_error(error, "没有热区达到高温核心阈值");
        return false;
    }
    std::sort(components.begin(), components.end(),
              [](const Component &first, const Component &second) {
                  if (first.peak_delta_c != second.peak_delta_c)
                      return first.peak_delta_c > second.peak_delta_c;
                  return first.weight > second.weight;
              });
    const Component &best = components.front();
    if (best.pixels < config.minimum_pixels) {
        set_error(error,
                  "热源高温核心像素过少：目标过小或距离过远");
        return false;
    }
    if (best.pixels > config.maximum_pixels) {
        set_error(error, "热源高温核心过大，不适合点标定");
        return false;
    }
    if (best.peak_delta_c < config.minimum_peak_delta_c) {
        set_error(error, "最热点温差不足，请使用更明显的暖目标");
        return false;
    }
    double secondary_ratio = 0.0;
    for (std::size_t index = 1; index < components.size(); ++index) {
        if (components[index].pixels >= config.minimum_pixels) {
            secondary_ratio = static_cast<double>(
                components[index].peak_delta_c) /
                static_cast<double>(best.peak_delta_c);
            break;
        }
    }
    if (secondary_ratio > config.maximum_secondary_peak_ratio) {
        set_error(error, "检测到两个温度接近的热目标");
        return false;
    }
    const double centroid_to_peak = std::hypot(
        best.centroid.x - best.peak.x,
        best.centroid.y - best.peak.y);
    if (centroid_to_peak >
        config.maximum_centroid_to_peak_distance_pixels) {
        set_error(error, "热质心距离最高温核心过远，拒绝采集");
        return false;
    }
    *hotspot = {
        best.centroid,
        best.peak,
        best.bounds,
        best.pixels,
        best.peak_delta_c,
        core_threshold,
        best.weight,
        secondary_ratio,
        centroid_to_peak,
    };
    return true;
}

bool summarize_stable_hotspots(
    const std::vector<ThermalHotspot> &samples,
    double maximum_allowed_deviation_pixels,
    StableHotspot *summary,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (summary == nullptr) {
        set_error(error, "stable hotspot output pointer is null");
        return false;
    }
    if (samples.empty() ||
        !std::isfinite(maximum_allowed_deviation_pixels) ||
        maximum_allowed_deviation_pixels < 0.0) {
        set_error(error, "stable hotspot input is invalid");
        return false;
    }
    double total_weight = 0.0;
    Point2d centroid;
    double peak_sum = 0.0;
    for (const ThermalHotspot &sample : samples) {
        if (!finite_point(sample.centroid) ||
            !std::isfinite(sample.total_weight) ||
            sample.total_weight <= 0.0 ||
            !std::isfinite(sample.peak_delta_c)) {
            set_error(error, "stable hotspot sample is invalid");
            return false;
        }
        total_weight += sample.total_weight;
        centroid.x += sample.centroid.x * sample.total_weight;
        centroid.y += sample.centroid.y * sample.total_weight;
        peak_sum += sample.peak_delta_c;
    }
    centroid.x /= total_weight;
    centroid.y /= total_weight;
    double maximum_deviation = 0.0;
    for (const ThermalHotspot &sample : samples) {
        maximum_deviation = std::max(
            maximum_deviation,
            std::hypot(sample.centroid.x - centroid.x,
                       sample.centroid.y - centroid.y));
    }
    if (maximum_deviation > maximum_allowed_deviation_pixels) {
        set_error(error, "thermal target moved during sample collection");
        return false;
    }
    *summary = {
        centroid,
        maximum_deviation,
        peak_sum / static_cast<double>(samples.size()),
        samples.size(),
    };
    return true;
}

bool calculate_mapping_errors(
    const std::array<double, 9> &thermal_to_visible,
    const std::vector<CrossSpectralCorrespondence> &validation_points,
    MappingErrorDistribution *distribution,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (distribution == nullptr) {
        set_error(error, "mapping error output pointer is null");
        return false;
    }
    if (validation_points.empty()) {
        set_error(error, "at least one validation point is required");
        return false;
    }
    for (const double value : thermal_to_visible) {
        if (!std::isfinite(value)) {
            set_error(error, "homography contains a non-finite value");
            return false;
        }
    }
    MappingErrorDistribution output;
    double squared_sum = 0.0;
    for (const CrossSpectralCorrespondence &point : validation_points) {
        if (!finite_point(point.thermal) || !finite_point(point.visible)) {
            set_error(error, "validation point contains a non-finite value");
            return false;
        }
        Point2d projected;
        if (!project_point(thermal_to_visible, point.thermal, &projected)) {
            set_error(error, "homography is undefined at a validation point");
            return false;
        }
        const double distance =
            std::hypot(projected.x - point.visible.x,
                       projected.y - point.visible.y);
        output.errors_px.push_back(distance);
        squared_sum += distance * distance;
    }
    std::sort(output.errors_px.begin(), output.errors_px.end());
    output.root_mean_square_px =
        std::sqrt(squared_sum /
                  static_cast<double>(output.errors_px.size()));
    output.p50_px = percentile(output.errors_px, 0.50);
    output.p95_px = percentile(output.errors_px, 0.95);
    output.maximum_px = output.errors_px.back();
    *distribution = std::move(output);
    return true;
}

}  // namespace p2
