#include "p2/calibration/cross_spectral.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

struct Options {
    std::string points;
    std::string output;
    std::string calibration_id;
    p2::Mlx90640Model model = p2::Mlx90640Model::unconfirmed;
    std::uint32_t visible_width = 0;
    std::uint32_t visible_height = 0;
    double nominal_distance_mm = 0.0;
    double minimum_distance_mm = 0.0;
    double maximum_distance_mm = 0.0;
    double maximum_allowed_error_px = 10.0;
};

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [required options]\n"
        << "  --points FILE              CSV: thermal_x,thermal_y,visible_x,visible_y\n"
        << "  --output FILE              calibrated .conf output\n"
        << "  --calibration-id ID        unique fixture/session id\n"
        << "  --model BAA|BAB            confirmed can marking\n"
        << "  --visible-width N          visible frame width\n"
        << "  --visible-height N         visible frame height\n"
        << "  --nominal-distance-mm V    calibration distance\n"
        << "  --min-distance-mm V        validated lower distance\n"
        << "  --max-distance-mm V        validated upper distance\n"
        << "  --max-error-px V           rejection gate, default 10\n";
}

bool parse_u32(const char *text, std::uint32_t *value)
{
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed > std::numeric_limits<std::uint32_t>::max())
        return false;
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_double(const char *text, double *value)
{
    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' ||
        !std::isfinite(parsed))
        return false;
    *value = parsed;
    return true;
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
        const char *value = argv[++index];
        if (argument == "--points")
            options->points = value;
        else if (argument == "--output")
            options->output = value;
        else if (argument == "--calibration-id")
            options->calibration_id = value;
        else if (argument == "--model") {
            const std::string model = value;
            if (model == "BAA")
                options->model = p2::Mlx90640Model::esf_baa;
            else if (model == "BAB")
                options->model = p2::Mlx90640Model::esf_bab;
            else
                return false;
        } else if (argument == "--visible-width") {
            if (!parse_u32(value, &options->visible_width))
                return false;
        } else if (argument == "--visible-height") {
            if (!parse_u32(value, &options->visible_height))
                return false;
        } else if (argument == "--nominal-distance-mm") {
            if (!parse_double(value, &options->nominal_distance_mm))
                return false;
        } else if (argument == "--min-distance-mm") {
            if (!parse_double(value, &options->minimum_distance_mm))
                return false;
        } else if (argument == "--max-distance-mm") {
            if (!parse_double(value, &options->maximum_distance_mm))
                return false;
        } else if (argument == "--max-error-px") {
            if (!parse_double(value, &options->maximum_allowed_error_px))
                return false;
        } else {
            return false;
        }
    }
    return !options->points.empty() && !options->output.empty() &&
        !options->calibration_id.empty() &&
        options->model != p2::Mlx90640Model::unconfirmed &&
        options->visible_width != 0 && options->visible_height != 0 &&
        options->minimum_distance_mm > 0.0 &&
        options->maximum_distance_mm >= options->minimum_distance_mm &&
        options->nominal_distance_mm >= options->minimum_distance_mm &&
        options->nominal_distance_mm <= options->maximum_distance_mm &&
        options->maximum_allowed_error_px >= 0.0;
}

bool parse_csv_point(const std::string &line,
                     p2::CrossSpectralCorrespondence *correspondence)
{
    std::stringstream input(line);
    std::string values[4];
    for (std::size_t index = 0; index < 4; ++index) {
        if (!std::getline(input, values[index], ','))
            return false;
    }
    std::string extra;
    if (std::getline(input, extra, ','))
        return false;
    return parse_double(values[0].c_str(), &correspondence->thermal.x) &&
        parse_double(values[1].c_str(), &correspondence->thermal.y) &&
        parse_double(values[2].c_str(), &correspondence->visible.x) &&
        parse_double(values[3].c_str(), &correspondence->visible.y);
}

bool load_points(const Options &options,
                 std::vector<p2::CrossSpectralCorrespondence> *points,
                 std::string *error)
{
    std::ifstream input(options.points);
    if (!input) {
        *error = "failed to open correspondence CSV";
        return false;
    }
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line.front() == '#')
            continue;
        if (line == "thermal_x,thermal_y,visible_x,visible_y")
            continue;
        p2::CrossSpectralCorrespondence point;
        if (!parse_csv_point(line, &point)) {
            *error = "invalid correspondence CSV line " +
                std::to_string(line_number);
            return false;
        }
        if (point.thermal.x < -0.5 || point.thermal.x > 31.5 ||
            point.thermal.y < -0.5 || point.thermal.y > 23.5 ||
            point.visible.x < -0.5 ||
            point.visible.x >
                static_cast<double>(options.visible_width) - 0.5 ||
            point.visible.y < -0.5 ||
            point.visible.y >
                static_cast<double>(options.visible_height) - 0.5) {
            *error = "correspondence outside declared image bounds on line " +
                std::to_string(line_number);
            return false;
        }
        points->push_back(point);
    }
    if (!input.eof()) {
        *error = "failed while reading correspondence CSV";
        return false;
    }
    if (points->size() < 4) {
        *error = "at least four correspondence rows are required";
        return false;
    }
    return true;
}

void write_summary(const Options &options, std::size_t point_count,
                   const p2::HomographyFitResult &fit, bool passed,
                   const std::string &error)
{
    const p2::FieldOfViewDegrees fov =
        p2::expected_field_of_view(options.model);
    std::cout << std::fixed << std::setprecision(6)
              << "{\n"
              << "  \"schema\": \"p2.cross-spectral-fit.v1\",\n"
              << "  \"result\": \"" << (passed ? "PASS" : "FAIL")
              << "\",\n"
              << "  \"error\": \"" << error << "\",\n"
              << "  \"application_commit\": \"" << P2_GIT_COMMIT
              << "\",\n"
              << "  \"calibration_id\": \"" << options.calibration_id
              << "\",\n"
              << "  \"thermal_model\": \""
              << p2::to_string(options.model) << "\",\n"
              << "  \"expected_fov_degrees\": [" << fov.horizontal
              << ", " << fov.vertical << "],\n"
              << "  \"correspondence_count\": " << point_count << ",\n"
              << "  \"root_mean_square_error_px\": "
              << fit.root_mean_square_error_px << ",\n"
              << "  \"max_reprojection_error_px\": "
              << fit.max_reprojection_error_px << ",\n"
              << "  \"maximum_allowed_error_px\": "
              << options.maximum_allowed_error_px << "\n}\n";
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse(argc, argv, &options)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    std::vector<p2::CrossSpectralCorrespondence> points;
    p2::HomographyFitResult fit;
    std::string error;
    bool passed = load_points(options, &points, &error);
    if (passed)
        passed = p2::fit_cross_spectral_homography(points, &fit, &error);
    if (passed &&
        fit.max_reprojection_error_px > options.maximum_allowed_error_px) {
        error = "maximum reprojection error exceeds the configured gate";
        passed = false;
    }

    p2::CrossSpectralCalibration calibration;
    if (passed) {
        calibration.schema = p2::kCrossSpectralCalibrationSchema;
        calibration.state = p2::CalibrationState::calibrated;
        calibration.calibration_id = options.calibration_id;
        calibration.thermal_model = options.model;
        calibration.thermal_width = p2::kThermalColumns;
        calibration.thermal_height = p2::kThermalRows;
        calibration.visible_width = options.visible_width;
        calibration.visible_height = options.visible_height;
        calibration.nominal_distance_mm = options.nominal_distance_mm;
        calibration.valid_distance_min_mm = options.minimum_distance_mm;
        calibration.valid_distance_max_mm = options.maximum_distance_mm;
        calibration.thermal_to_visible = fit.thermal_to_visible;
        calibration.max_reprojection_error_px =
            fit.max_reprojection_error_px;
        std::ofstream output(options.output);
        if (!output || !p2::write_cross_spectral_calibration(
                           output, calibration, &error))
            passed = false;
    }
    write_summary(options, points.size(), fit, passed, error);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
