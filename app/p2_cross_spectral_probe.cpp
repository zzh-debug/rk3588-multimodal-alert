#include "p2/calibration/cross_spectral.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

struct Options {
    std::string config;
    std::string output;
    p2::ThermalIndexRegion thermal_region{0, 0, 31, 23};
    p2::Rect2d visible_roi{};
    double minimum_overlap = 0.0;
    bool have_visible_roi = false;
};

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " --config FILE [options]\n"
        << "  --thermal-region C0,R0,C1,R1  inclusive, default full 32x24\n"
        << "  --visible-roi X0,Y0,X1,Y1    optional visible detection ROI\n"
        << "  --min-overlap VALUE           required overlap, default 0\n"
        << "  --output FILE                 optional JSON output\n";
}

bool parse_u32(const std::string &text, std::uint32_t *value)
{
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<std::uint32_t>::max())
        return false;
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_double(const std::string &text, double *value)
{
    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        !std::isfinite(parsed))
        return false;
    *value = parsed;
    return true;
}

bool split_four(const std::string &text, std::string values[4])
{
    std::stringstream input(text);
    for (std::size_t index = 0; index < 4; ++index) {
        if (!std::getline(input, values[index], ','))
            return false;
    }
    std::string extra;
    return !std::getline(input, extra, ',');
}

bool parse_thermal_region(const std::string &text,
                          p2::ThermalIndexRegion *region)
{
    std::string values[4];
    return split_four(text, values) &&
        parse_u32(values[0], &region->first_column) &&
        parse_u32(values[1], &region->first_row) &&
        parse_u32(values[2], &region->last_column) &&
        parse_u32(values[3], &region->last_row);
}

bool parse_rectangle(const std::string &text, p2::Rect2d *rectangle)
{
    std::string values[4];
    return split_four(text, values) &&
        parse_double(values[0], &rectangle->x_min) &&
        parse_double(values[1], &rectangle->y_min) &&
        parse_double(values[2], &rectangle->x_max) &&
        parse_double(values[3], &rectangle->y_max);
}

bool parse(int argc, char **argv, Options *options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (index + 1 >= argc)
            return false;
        const std::string value = argv[++index];
        if (argument == "--config")
            options->config = value;
        else if (argument == "--output")
            options->output = value;
        else if (argument == "--thermal-region") {
            if (!parse_thermal_region(value, &options->thermal_region))
                return false;
        } else if (argument == "--visible-roi") {
            if (!parse_rectangle(value, &options->visible_roi))
                return false;
            options->have_visible_roi = true;
        } else if (argument == "--min-overlap") {
            if (!parse_double(value, &options->minimum_overlap) ||
                options->minimum_overlap < 0.0 ||
                options->minimum_overlap > 1.0)
                return false;
        } else {
            return false;
        }
    }
    return !options->config.empty() &&
        (options->minimum_overlap == 0.0 || options->have_visible_roi);
}

std::string json_escape(const std::string &value)
{
    std::string escaped;
    for (const char character : value) {
        if (character == '\\' || character == '"')
            escaped.push_back('\\');
        if (character == '\n') {
            escaped += "\\n";
            continue;
        }
        escaped.push_back(character);
    }
    return escaped;
}

void write_point(std::ostream &output, const p2::Point2d &point)
{
    output << "[" << point.x << ", " << point.y << "]";
}

void write_rect(std::ostream &output, const p2::Rect2d &rectangle)
{
    output << "[" << rectangle.x_min << ", " << rectangle.y_min
           << ", " << rectangle.x_max << ", " << rectangle.y_max
           << "]";
}

void write_report(std::ostream &output, const Options &options,
                  const p2::CrossSpectralCalibration &calibration,
                  const p2::MappedThermalRegion &mapped,
                  double overlap, bool passed, const std::string &error)
{
    const p2::FieldOfViewDegrees fov =
        p2::expected_field_of_view(calibration.thermal_model);
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"schema\": \"p2.cross-spectral-probe.v1\",\n"
           << "  \"result\": \"" << (passed ? "PASS" : "FAIL")
           << "\",\n"
           << "  \"error\": \"" << json_escape(error) << "\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT
           << "\",\n"
           << "  \"calibration_id\": \""
           << json_escape(calibration.calibration_id) << "\",\n"
           << "  \"thermal_model\": \""
           << p2::to_string(calibration.thermal_model) << "\",\n"
           << "  \"expected_fov_degrees\": [" << fov.horizontal
           << ", " << fov.vertical << "],\n"
           << "  \"nominal_distance_mm\": "
           << calibration.nominal_distance_mm << ",\n"
           << "  \"thermal_region_inclusive\": ["
           << options.thermal_region.first_column << ", "
           << options.thermal_region.first_row << ", "
           << options.thermal_region.last_column << ", "
           << options.thermal_region.last_row << "],\n"
           << "  \"visible_quadrilateral\": [";
    for (std::size_t index = 0;
         index < mapped.visible_quadrilateral.size(); ++index) {
        if (index != 0)
            output << ", ";
        write_point(output, mapped.visible_quadrilateral[index]);
    }
    output << "],\n  \"visible_bounds\": ";
    write_rect(output, mapped.visible_bounds);
    output << ",\n  \"clipped_visible_bounds\": ";
    write_rect(output, mapped.clipped_visible_bounds);
    output << ",\n"
           << "  \"intersects_visible_image\": "
           << (mapped.intersects_visible_image ? "true" : "false")
           << ",\n"
           << "  \"visible_roi_provided\": "
           << (options.have_visible_roi ? "true" : "false") << ",\n"
           << "  \"intersection_over_thermal_area\": " << overlap
           << ",\n"
           << "  \"minimum_overlap\": " << options.minimum_overlap
           << "\n}\n";
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse(argc, argv, &options)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    p2::CrossSpectralCalibration calibration;
    p2::MappedThermalRegion mapped;
    std::string error;
    bool passed = p2::load_cross_spectral_calibration(
        options.config, &calibration, &error);
    if (passed)
        passed = p2::map_thermal_region(
            calibration, options.thermal_region, &mapped, &error);
    double overlap = 0.0;
    if (passed && options.have_visible_roi) {
        overlap = p2::intersection_over_first_area(
            mapped.clipped_visible_bounds, options.visible_roi);
        if (overlap < options.minimum_overlap) {
            error = "mapped region overlap is below the requested threshold";
            passed = false;
        }
    }

    write_report(std::cout, options, calibration, mapped, overlap,
                 passed, error);
    if (!options.output.empty()) {
        std::ofstream output(options.output);
        if (!output) {
            std::cerr << "failed to open report " << options.output << '\n';
            return EXIT_FAILURE;
        }
        write_report(output, options, calibration, mapped, overlap,
                     passed, error);
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
