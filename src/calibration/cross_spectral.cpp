#include "p2/calibration/cross_spectral.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace p2 {
namespace {

constexpr double kDenominatorEpsilon = 1.0e-12;

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr)
        *error = message;
}

std::string trim(const std::string &value)
{
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_u32(const std::string &text, std::uint32_t *value)
{
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<std::uint32_t>::max())
        return false;
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_finite_double(const std::string &text, double *value)
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

bool parse_homography(const std::string &text,
                      std::array<double, 9> *homography)
{
    std::stringstream stream(text);
    std::string token;
    std::size_t index = 0;
    while (std::getline(stream, token, ',')) {
        if (index >= homography->size() ||
            !parse_finite_double(trim(token), &(*homography)[index]))
            return false;
        ++index;
    }
    return index == homography->size();
}

bool parse_state(const std::string &text, CalibrationState *state)
{
    if (text == "uncalibrated") {
        *state = CalibrationState::uncalibrated;
        return true;
    }
    if (text == "calibrated") {
        *state = CalibrationState::calibrated;
        return true;
    }
    return false;
}

bool parse_model(const std::string &text, Mlx90640Model *model)
{
    if (text == "UNCONFIRMED") {
        *model = Mlx90640Model::unconfirmed;
        return true;
    }
    if (text == "MLX90640-ESF-BAA") {
        *model = Mlx90640Model::esf_baa;
        return true;
    }
    if (text == "MLX90640-ESF-BAB") {
        *model = Mlx90640Model::esf_bab;
        return true;
    }
    return false;
}

double determinant(const std::array<double, 9> &matrix)
{
    return matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7]) -
        matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6]) +
        matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
}

bool project_unchecked(const std::array<double, 9> &matrix,
                       const Point2d &source, Point2d *destination)
{
    const double denominator =
        matrix[6] * source.x + matrix[7] * source.y + matrix[8];
    if (!std::isfinite(denominator) ||
        std::fabs(denominator) <= kDenominatorEpsilon)
        return false;
    destination->x =
        (matrix[0] * source.x + matrix[1] * source.y + matrix[2]) /
        denominator;
    destination->y =
        (matrix[3] * source.x + matrix[4] * source.y + matrix[5]) /
        denominator;
    return std::isfinite(destination->x) && std::isfinite(destination->y);
}

bool valid_rectangle(const Rect2d &rectangle)
{
    return std::isfinite(rectangle.x_min) &&
        std::isfinite(rectangle.y_min) &&
        std::isfinite(rectangle.x_max) &&
        std::isfinite(rectangle.y_max) &&
        rectangle.x_max > rectangle.x_min &&
        rectangle.y_max > rectangle.y_min;
}

bool valid_identifier(const std::string &identifier)
{
    if (identifier.empty())
        return false;
    for (const unsigned char character : identifier) {
        if (!std::isalnum(character) && character != '-' &&
            character != '_' && character != '.')
            return false;
    }
    return true;
}

using Matrix3 = std::array<double, 9>;

Matrix3 multiply(const Matrix3 &first, const Matrix3 &second)
{
    Matrix3 result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            for (std::size_t inner = 0; inner < 3; ++inner) {
                result[row * 3 + column] +=
                    first[row * 3 + inner] *
                    second[inner * 3 + column];
            }
        }
    }
    return result;
}

struct PointNormalization {
    std::vector<Point2d> points;
    Matrix3 transform{};
    Matrix3 inverse_transform{};
};

bool normalize_points(const std::vector<Point2d> &points,
                      PointNormalization *normalization)
{
    Point2d mean;
    for (const Point2d &point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y))
            return false;
        mean.x += point.x;
        mean.y += point.y;
    }
    mean.x /= static_cast<double>(points.size());
    mean.y /= static_cast<double>(points.size());
    double mean_distance = 0.0;
    for (const Point2d &point : points) {
        mean_distance += std::hypot(point.x - mean.x, point.y - mean.y);
    }
    mean_distance /= static_cast<double>(points.size());
    if (!std::isfinite(mean_distance) ||
        mean_distance <= kDenominatorEpsilon)
        return false;
    const double scale = std::sqrt(2.0) / mean_distance;
    normalization->transform = {
        scale, 0.0, -scale * mean.x,
        0.0, scale, -scale * mean.y,
        0.0, 0.0, 1.0,
    };
    normalization->inverse_transform = {
        1.0 / scale, 0.0, mean.x,
        0.0, 1.0 / scale, mean.y,
        0.0, 0.0, 1.0,
    };
    normalization->points.reserve(points.size());
    for (const Point2d &point : points) {
        normalization->points.push_back({
            scale * (point.x - mean.x),
            scale * (point.y - mean.y),
        });
    }
    return true;
}

bool solve_linear_system(std::array<std::array<double, 9>, 8> *matrix,
                         std::array<double, 8> *solution)
{
    for (std::size_t column = 0; column < 8; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 8; ++row) {
            if (std::fabs((*matrix)[row][column]) >
                std::fabs((*matrix)[pivot][column]))
                pivot = row;
        }
        if (std::fabs((*matrix)[pivot][column]) <=
            kDenominatorEpsilon)
            return false;
        if (pivot != column)
            std::swap((*matrix)[pivot], (*matrix)[column]);
        const double divisor = (*matrix)[column][column];
        for (std::size_t index = column; index < 9; ++index)
            (*matrix)[column][index] /= divisor;
        for (std::size_t row = 0; row < 8; ++row) {
            if (row == column)
                continue;
            const double factor = (*matrix)[row][column];
            for (std::size_t index = column; index < 9; ++index)
                (*matrix)[row][index] -= factor * (*matrix)[column][index];
        }
    }
    for (std::size_t row = 0; row < 8; ++row)
        (*solution)[row] = (*matrix)[row][8];
    return true;
}

}  // namespace

const char *to_string(CalibrationState state)
{
    return state == CalibrationState::calibrated
        ? "calibrated"
        : "uncalibrated";
}

const char *to_string(Mlx90640Model model)
{
    switch (model) {
    case Mlx90640Model::esf_baa:
        return "MLX90640-ESF-BAA";
    case Mlx90640Model::esf_bab:
        return "MLX90640-ESF-BAB";
    case Mlx90640Model::unconfirmed:
        return "UNCONFIRMED";
    }
    return "UNCONFIRMED";
}

FieldOfViewDegrees expected_field_of_view(Mlx90640Model model)
{
    switch (model) {
    case Mlx90640Model::esf_baa:
        return {110.0, 75.0};
    case Mlx90640Model::esf_bab:
        return {55.0, 35.0};
    case Mlx90640Model::unconfirmed:
        return {};
    }
    return {};
}

bool parse_cross_spectral_calibration(
    std::istream &input,
    CrossSpectralCalibration *calibration,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (calibration == nullptr) {
        set_error(error, "calibration output pointer is null");
        return false;
    }

    static const std::unordered_set<std::string> kAllowedKeys{
        "schema", "state", "calibration_id", "thermal_model",
        "thermal_width", "thermal_height", "visible_width",
        "visible_height", "nominal_distance_mm",
        "valid_distance_min_mm", "valid_distance_max_mm",
        "homography", "max_reprojection_error_px",
    };
    std::unordered_map<std::string, std::string> values;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#')
            continue;
        const std::size_t separator = stripped.find('=');
        if (separator == std::string::npos) {
            set_error(error, "missing '=' on calibration line " +
                std::to_string(line_number));
            return false;
        }
        const std::string key = trim(stripped.substr(0, separator));
        const std::string value = trim(stripped.substr(separator + 1));
        if (kAllowedKeys.count(key) == 0) {
            set_error(error, "unknown calibration key '" + key + "'");
            return false;
        }
        if (value.empty()) {
            set_error(error, "empty calibration value for '" + key + "'");
            return false;
        }
        if (!values.emplace(key, value).second) {
            set_error(error, "duplicate calibration key '" + key + "'");
            return false;
        }
    }
    if (!input.eof()) {
        set_error(error, "failed while reading calibration stream");
        return false;
    }
    for (const std::string &key : kAllowedKeys) {
        if (values.count(key) == 0) {
            set_error(error, "missing calibration key '" + key + "'");
            return false;
        }
    }

    CrossSpectralCalibration parsed;
    parsed.schema = values["schema"];
    parsed.calibration_id = values["calibration_id"];
    if (!parse_state(values["state"], &parsed.state) ||
        !parse_model(values["thermal_model"], &parsed.thermal_model) ||
        !parse_u32(values["thermal_width"], &parsed.thermal_width) ||
        !parse_u32(values["thermal_height"], &parsed.thermal_height) ||
        !parse_u32(values["visible_width"], &parsed.visible_width) ||
        !parse_u32(values["visible_height"], &parsed.visible_height) ||
        !parse_finite_double(values["nominal_distance_mm"],
                             &parsed.nominal_distance_mm) ||
        !parse_finite_double(values["valid_distance_min_mm"],
                             &parsed.valid_distance_min_mm) ||
        !parse_finite_double(values["valid_distance_max_mm"],
                             &parsed.valid_distance_max_mm) ||
        !parse_homography(values["homography"],
                          &parsed.thermal_to_visible) ||
        !parse_finite_double(values["max_reprojection_error_px"],
                             &parsed.max_reprojection_error_px)) {
        set_error(error, "calibration contains an invalid typed value");
        return false;
    }
    *calibration = std::move(parsed);
    return true;
}

bool load_cross_spectral_calibration(
    const std::string &path,
    CrossSpectralCalibration *calibration,
    std::string *error)
{
    std::ifstream input(path);
    if (!input) {
        set_error(error, "failed to open calibration file '" + path + "'");
        return false;
    }
    return parse_cross_spectral_calibration(input, calibration, error);
}

bool write_cross_spectral_calibration(
    std::ostream &output,
    const CrossSpectralCalibration &calibration,
    std::string *error)
{
    if (!validate_cross_spectral_calibration(calibration, error))
        return false;
    output << std::setprecision(17)
           << "schema=" << calibration.schema << '\n'
           << "state=" << to_string(calibration.state) << '\n'
           << "calibration_id=" << calibration.calibration_id << '\n'
           << "thermal_model=" << to_string(calibration.thermal_model)
           << '\n'
           << "thermal_width=" << calibration.thermal_width << '\n'
           << "thermal_height=" << calibration.thermal_height << '\n'
           << "visible_width=" << calibration.visible_width << '\n'
           << "visible_height=" << calibration.visible_height << '\n'
           << "nominal_distance_mm="
           << calibration.nominal_distance_mm << '\n'
           << "valid_distance_min_mm="
           << calibration.valid_distance_min_mm << '\n'
           << "valid_distance_max_mm="
           << calibration.valid_distance_max_mm << '\n'
           << "homography=";
    for (std::size_t index = 0;
         index < calibration.thermal_to_visible.size(); ++index) {
        if (index != 0)
            output << ',';
        output << calibration.thermal_to_visible[index];
    }
    output << '\n'
           << "max_reprojection_error_px="
           << calibration.max_reprojection_error_px << '\n';
    if (!output) {
        set_error(error, "failed while writing calibration stream");
        return false;
    }
    return true;
}

bool validate_cross_spectral_calibration(
    const CrossSpectralCalibration &calibration,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (calibration.schema != kCrossSpectralCalibrationSchema) {
        set_error(error, "unsupported calibration schema");
        return false;
    }
    if (calibration.state != CalibrationState::calibrated) {
        set_error(error, "calibration state is not calibrated");
        return false;
    }
    if (!valid_identifier(calibration.calibration_id) ||
        calibration.calibration_id == "replace-me") {
        set_error(error, "calibration id is invalid or still a placeholder");
        return false;
    }
    if (calibration.thermal_model == Mlx90640Model::unconfirmed) {
        set_error(error, "MLX90640 BAA/BAB model is unconfirmed");
        return false;
    }
    if (calibration.thermal_width != kThermalColumns ||
        calibration.thermal_height != kThermalRows) {
        set_error(error, "thermal dimensions must be 32x24");
        return false;
    }
    if (calibration.visible_width == 0 || calibration.visible_height == 0) {
        set_error(error, "visible dimensions must be non-zero");
        return false;
    }
    if (!std::isfinite(calibration.nominal_distance_mm) ||
        !std::isfinite(calibration.valid_distance_min_mm) ||
        !std::isfinite(calibration.valid_distance_max_mm) ||
        calibration.valid_distance_min_mm <= 0.0 ||
        calibration.valid_distance_max_mm <
            calibration.valid_distance_min_mm ||
        calibration.nominal_distance_mm <
            calibration.valid_distance_min_mm ||
        calibration.nominal_distance_mm >
            calibration.valid_distance_max_mm) {
        set_error(error, "calibration distance range is invalid");
        return false;
    }
    if (!std::isfinite(calibration.max_reprojection_error_px) ||
        calibration.max_reprojection_error_px < 0.0) {
        set_error(error, "reprojection error is invalid");
        return false;
    }
    for (const double value : calibration.thermal_to_visible) {
        if (!std::isfinite(value)) {
            set_error(error, "homography contains a non-finite value");
            return false;
        }
    }
    if (std::fabs(determinant(calibration.thermal_to_visible)) <=
        kDenominatorEpsilon) {
        set_error(error, "homography is singular");
        return false;
    }
    const std::array<Point2d, 4> thermal_corners{{
        {-0.5, -0.5},
        {static_cast<double>(kThermalColumns) - 0.5, -0.5},
        {static_cast<double>(kThermalColumns) - 0.5,
         static_cast<double>(kThermalRows) - 0.5},
        {-0.5, static_cast<double>(kThermalRows) - 0.5},
    }};
    Point2d projected;
    for (const Point2d &corner : thermal_corners) {
        if (!project_unchecked(calibration.thermal_to_visible, corner,
                               &projected)) {
            set_error(error, "homography is undefined at a thermal corner");
            return false;
        }
    }
    return true;
}

bool project_thermal_point(
    const CrossSpectralCalibration &calibration,
    const Point2d &thermal_point,
    Point2d *visible_point,
    std::string *error)
{
    if (visible_point == nullptr) {
        set_error(error, "visible point output pointer is null");
        return false;
    }
    if (!std::isfinite(thermal_point.x) ||
        !std::isfinite(thermal_point.y)) {
        set_error(error, "thermal point is non-finite");
        return false;
    }
    if (!validate_cross_spectral_calibration(calibration, error))
        return false;
    if (!project_unchecked(calibration.thermal_to_visible, thermal_point,
                           visible_point)) {
        set_error(error, "homography is undefined at the thermal point");
        return false;
    }
    return true;
}

bool map_thermal_region(
    const CrossSpectralCalibration &calibration,
    const ThermalIndexRegion &thermal_region,
    MappedThermalRegion *mapped,
    std::string *error)
{
    if (mapped == nullptr) {
        set_error(error, "mapped region output pointer is null");
        return false;
    }
    if (!validate_cross_spectral_calibration(calibration, error))
        return false;
    if (thermal_region.first_column > thermal_region.last_column ||
        thermal_region.first_row > thermal_region.last_row ||
        thermal_region.last_column >= calibration.thermal_width ||
        thermal_region.last_row >= calibration.thermal_height) {
        set_error(error, "thermal index region is invalid");
        return false;
    }

    const double left =
        static_cast<double>(thermal_region.first_column) - 0.5;
    const double top = static_cast<double>(thermal_region.first_row) - 0.5;
    const double right =
        static_cast<double>(thermal_region.last_column) + 0.5;
    const double bottom =
        static_cast<double>(thermal_region.last_row) + 0.5;
    const std::array<Point2d, 4> thermal_corners{{
        {left, top}, {right, top}, {right, bottom}, {left, bottom},
    }};

    MappedThermalRegion output;
    for (std::size_t index = 0; index < thermal_corners.size(); ++index) {
        if (!project_unchecked(calibration.thermal_to_visible,
                               thermal_corners[index],
                               &output.visible_quadrilateral[index])) {
            set_error(error, "homography is undefined in thermal region");
            return false;
        }
    }
    output.visible_bounds = {
        output.visible_quadrilateral[0].x,
        output.visible_quadrilateral[0].y,
        output.visible_quadrilateral[0].x,
        output.visible_quadrilateral[0].y,
    };
    for (const Point2d &point : output.visible_quadrilateral) {
        output.visible_bounds.x_min =
            std::min(output.visible_bounds.x_min, point.x);
        output.visible_bounds.y_min =
            std::min(output.visible_bounds.y_min, point.y);
        output.visible_bounds.x_max =
            std::max(output.visible_bounds.x_max, point.x);
        output.visible_bounds.y_max =
            std::max(output.visible_bounds.y_max, point.y);
    }
    const double visible_x_max =
        static_cast<double>(calibration.visible_width - 1);
    const double visible_y_max =
        static_cast<double>(calibration.visible_height - 1);
    output.intersects_visible_image =
        output.visible_bounds.x_max >= 0.0 &&
        output.visible_bounds.y_max >= 0.0 &&
        output.visible_bounds.x_min <= visible_x_max &&
        output.visible_bounds.y_min <= visible_y_max;
    output.clipped_visible_bounds = {
        std::clamp(output.visible_bounds.x_min, 0.0, visible_x_max),
        std::clamp(output.visible_bounds.y_min, 0.0, visible_y_max),
        std::clamp(output.visible_bounds.x_max, 0.0, visible_x_max),
        std::clamp(output.visible_bounds.y_max, 0.0, visible_y_max),
    };
    *mapped = output;
    return true;
}

bool fit_cross_spectral_homography(
    const std::vector<CrossSpectralCorrespondence> &correspondences,
    HomographyFitResult *result,
    std::string *error)
{
    if (error != nullptr)
        error->clear();
    if (result == nullptr) {
        set_error(error, "homography fit output pointer is null");
        return false;
    }
    if (correspondences.size() < 4) {
        set_error(error, "at least four correspondences are required");
        return false;
    }
    std::vector<Point2d> thermal_points;
    std::vector<Point2d> visible_points;
    thermal_points.reserve(correspondences.size());
    visible_points.reserve(correspondences.size());
    for (const CrossSpectralCorrespondence &correspondence :
         correspondences) {
        thermal_points.push_back(correspondence.thermal);
        visible_points.push_back(correspondence.visible);
    }
    PointNormalization thermal_normalization;
    PointNormalization visible_normalization;
    if (!normalize_points(thermal_points, &thermal_normalization) ||
        !normalize_points(visible_points, &visible_normalization)) {
        set_error(error, "correspondences are non-finite or degenerate");
        return false;
    }

    std::array<std::array<double, 8>, 8> normal{};
    std::array<double, 8> right_hand_side{};
    for (std::size_t index = 0; index < correspondences.size(); ++index) {
        const double x = thermal_normalization.points[index].x;
        const double y = thermal_normalization.points[index].y;
        const double u = visible_normalization.points[index].x;
        const double v = visible_normalization.points[index].y;
        const std::array<std::array<double, 8>, 2> rows{{
            {{x, y, 1.0, 0.0, 0.0, 0.0, -u * x, -u * y}},
            {{0.0, 0.0, 0.0, x, y, 1.0, -v * x, -v * y}},
        }};
        const std::array<double, 2> observations{{u, v}};
        for (std::size_t equation = 0; equation < rows.size(); ++equation) {
            for (std::size_t row = 0; row < 8; ++row) {
                right_hand_side[row] +=
                    rows[equation][row] * observations[equation];
                for (std::size_t column = 0; column < 8; ++column) {
                    normal[row][column] +=
                        rows[equation][row] * rows[equation][column];
                }
            }
        }
    }
    std::array<std::array<double, 9>, 8> augmented{};
    for (std::size_t row = 0; row < 8; ++row) {
        for (std::size_t column = 0; column < 8; ++column)
            augmented[row][column] = normal[row][column];
        augmented[row][8] = right_hand_side[row];
    }
    std::array<double, 8> coefficients{};
    if (!solve_linear_system(&augmented, &coefficients)) {
        set_error(error, "correspondences do not define a stable homography");
        return false;
    }
    Matrix3 normalized_homography{
        coefficients[0], coefficients[1], coefficients[2],
        coefficients[3], coefficients[4], coefficients[5],
        coefficients[6], coefficients[7], 1.0,
    };
    Matrix3 homography = multiply(
        visible_normalization.inverse_transform,
        multiply(normalized_homography, thermal_normalization.transform));
    if (std::fabs(homography[8]) <= kDenominatorEpsilon) {
        set_error(error, "fitted homography has an invalid scale");
        return false;
    }
    const double homography_scale = homography[8];
    for (double &value : homography)
        value /= homography_scale;

    HomographyFitResult output;
    output.thermal_to_visible = homography;
    double squared_error_sum = 0.0;
    for (const CrossSpectralCorrespondence &correspondence :
         correspondences) {
        Point2d projected;
        if (!project_unchecked(homography, correspondence.thermal,
                               &projected)) {
            set_error(error, "fitted homography is undefined at an input");
            return false;
        }
        const double distance = std::hypot(
            projected.x - correspondence.visible.x,
            projected.y - correspondence.visible.y);
        squared_error_sum += distance * distance;
        output.max_reprojection_error_px =
            std::max(output.max_reprojection_error_px, distance);
    }
    output.root_mean_square_error_px = std::sqrt(
        squared_error_sum / static_cast<double>(correspondences.size()));
    *result = output;
    return true;
}

double rectangle_area(const Rect2d &rectangle)
{
    if (!valid_rectangle(rectangle))
        return 0.0;
    return (rectangle.x_max - rectangle.x_min) *
        (rectangle.y_max - rectangle.y_min);
}

double intersection_over_first_area(
    const Rect2d &first,
    const Rect2d &second)
{
    const double first_area = rectangle_area(first);
    if (first_area <= 0.0 || !valid_rectangle(second))
        return 0.0;
    const Rect2d intersection{
        std::max(first.x_min, second.x_min),
        std::max(first.y_min, second.y_min),
        std::min(first.x_max, second.x_max),
        std::min(first.y_max, second.y_max),
    };
    return rectangle_area(intersection) / first_area;
}

}  // namespace p2
