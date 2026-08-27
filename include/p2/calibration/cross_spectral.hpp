#pragma once

#include <array>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace p2 {

inline constexpr const char *kCrossSpectralCalibrationSchema =
    "p2.cross-spectral-calibration.v1";
inline constexpr std::uint32_t kThermalColumns = 32;
inline constexpr std::uint32_t kThermalRows = 24;

enum class CalibrationState {
    uncalibrated,
    calibrated,
};

enum class Mlx90640Model {
    unconfirmed,
    esf_baa,
    esf_bab,
};

struct FieldOfViewDegrees {
    double horizontal = 0.0;
    double vertical = 0.0;
};

struct CrossSpectralCalibration {
    std::string schema;
    CalibrationState state = CalibrationState::uncalibrated;
    std::string calibration_id;
    Mlx90640Model thermal_model = Mlx90640Model::unconfirmed;
    std::uint32_t thermal_width = 0;
    std::uint32_t thermal_height = 0;
    std::uint32_t visible_width = 0;
    std::uint32_t visible_height = 0;
    double nominal_distance_mm = 0.0;
    double valid_distance_min_mm = 0.0;
    double valid_distance_max_mm = 0.0;
    std::array<double, 9> thermal_to_visible{};
    double max_reprojection_error_px = 0.0;
};

struct Point2d {
    double x = 0.0;
    double y = 0.0;
};

struct Rect2d {
    double x_min = 0.0;
    double y_min = 0.0;
    double x_max = 0.0;
    double y_max = 0.0;
};

struct ThermalIndexRegion {
    std::uint32_t first_column = 0;
    std::uint32_t first_row = 0;
    std::uint32_t last_column = 0;
    std::uint32_t last_row = 0;
};

struct MappedThermalRegion {
    std::array<Point2d, 4> visible_quadrilateral{};
    Rect2d visible_bounds{};
    Rect2d clipped_visible_bounds{};
    bool intersects_visible_image = false;
};

struct CrossSpectralCorrespondence {
    Point2d thermal;
    Point2d visible;
};

struct HomographyFitResult {
    std::array<double, 9> thermal_to_visible{};
    double root_mean_square_error_px = 0.0;
    double max_reprojection_error_px = 0.0;
};

const char *to_string(CalibrationState state);
const char *to_string(Mlx90640Model model);
FieldOfViewDegrees expected_field_of_view(Mlx90640Model model);

bool parse_cross_spectral_calibration(
    std::istream &input,
    CrossSpectralCalibration *calibration,
    std::string *error);
bool load_cross_spectral_calibration(
    const std::string &path,
    CrossSpectralCalibration *calibration,
    std::string *error);
bool write_cross_spectral_calibration(
    std::ostream &output,
    const CrossSpectralCalibration &calibration,
    std::string *error);
bool validate_cross_spectral_calibration(
    const CrossSpectralCalibration &calibration,
    std::string *error);

bool project_thermal_point(
    const CrossSpectralCalibration &calibration,
    const Point2d &thermal_point,
    Point2d *visible_point,
    std::string *error);
bool map_thermal_region(
    const CrossSpectralCalibration &calibration,
    const ThermalIndexRegion &thermal_region,
    MappedThermalRegion *mapped,
    std::string *error);
bool fit_cross_spectral_homography(
    const std::vector<CrossSpectralCorrespondence> &correspondences,
    HomographyFitResult *result,
    std::string *error);

double rectangle_area(const Rect2d &rectangle);
double intersection_over_first_area(
    const Rect2d &first,
    const Rect2d &second);

}  // namespace p2
