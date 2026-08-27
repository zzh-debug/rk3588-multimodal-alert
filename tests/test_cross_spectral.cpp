#include "p2/calibration/cross_spectral.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
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

const char *kValidCalibration = R"CAL(
schema=p2.cross-spectral-calibration.v1
state=calibrated
calibration_id=synthetic-2026-08-27
thermal_model=MLX90640-ESF-BAA
thermal_width=32
thermal_height=24
visible_width=3840
visible_height=2160
nominal_distance_mm=1500
valid_distance_min_mm=1000
valid_distance_max_mm=2000
homography=120,0,60,0,85,80,0,0,1
max_reprojection_error_px=2.5
)CAL";

}  // namespace

int main()
{
    std::string error;
    p2::CrossSpectralCalibration calibration;
    std::istringstream input(kValidCalibration);
    if (!require(p2::parse_cross_spectral_calibration(
                     input, &calibration, &error),
                 "valid calibration parses") ||
        !require(p2::validate_cross_spectral_calibration(
                     calibration, &error),
                 "valid calibration passes use gate") ||
        !require(p2::expected_field_of_view(calibration.thermal_model)
                         .horizontal == 110.0,
                 "BAA FOV comes from official model mapping"))
        return EXIT_FAILURE;

    p2::Point2d visible;
    if (!require(p2::project_thermal_point(
                     calibration, {2.0, 4.0}, &visible, &error),
                 "point projection succeeds") ||
        !require(near(visible.x, 300.0) && near(visible.y, 420.0),
                 "point projection applies homography"))
        return EXIT_FAILURE;

    p2::MappedThermalRegion mapped;
    if (!require(p2::map_thermal_region(
                     calibration, {2, 4, 3, 5}, &mapped, &error),
                 "inclusive thermal region maps") ||
        !require(near(mapped.visible_bounds.x_min, 240.0) &&
                     near(mapped.visible_bounds.x_max, 480.0) &&
                     near(mapped.visible_bounds.y_min, 377.5) &&
                     near(mapped.visible_bounds.y_max, 547.5),
                 "region uses outer pixel-cell corners") ||
        !require(mapped.intersects_visible_image,
                 "mapped region intersects visible image"))
        return EXIT_FAILURE;

    const p2::Rect2d half_region{
        mapped.visible_bounds.x_min,
        mapped.visible_bounds.y_min,
        (mapped.visible_bounds.x_min + mapped.visible_bounds.x_max) / 2.0,
        mapped.visible_bounds.y_max,
    };
    if (!require(near(p2::intersection_over_first_area(
                          mapped.visible_bounds, half_region),
                      0.5),
                 "intersection-over-thermal-area is deterministic"))
        return EXIT_FAILURE;

    p2::CrossSpectralCalibration rejected = calibration;
    rejected.state = p2::CalibrationState::uncalibrated;
    if (!require(!p2::validate_cross_spectral_calibration(
                     rejected, &error),
                 "uncalibrated template cannot be used"))
        return EXIT_FAILURE;
    rejected = calibration;
    rejected.thermal_model = p2::Mlx90640Model::unconfirmed;
    if (!require(!p2::validate_cross_spectral_calibration(
                     rejected, &error),
                 "unconfirmed model cannot be used"))
        return EXIT_FAILURE;
    rejected = calibration;
    rejected.thermal_to_visible.fill(0.0);
    if (!require(!p2::validate_cross_spectral_calibration(
                     rejected, &error),
                 "singular homography rejected"))
        return EXIT_FAILURE;

    std::istringstream duplicate(
        std::string(kValidCalibration) + "schema=duplicate\n");
    if (!require(!p2::parse_cross_spectral_calibration(
                     duplicate, &rejected, &error),
                 "duplicate key rejected"))
        return EXIT_FAILURE;
    std::istringstream unknown(
        std::string(kValidCalibration) + "fov_guess=110\n");
    if (!require(!p2::parse_cross_spectral_calibration(
                     unknown, &rejected, &error),
                 "unknown key rejected"))
        return EXIT_FAILURE;
    if (!require(!p2::map_thermal_region(
                     calibration, {0, 0, 32, 23}, &mapped, &error),
                 "out-of-range thermal region rejected"))
        return EXIT_FAILURE;

    const std::vector<p2::CrossSpectralCorrespondence> correspondences{
        {{0.0, 0.0}, {60.0, 80.0}},
        {{31.0, 0.0}, {3780.0, 80.0}},
        {{31.0, 23.0}, {3780.0, 2035.0}},
        {{0.0, 23.0}, {60.0, 2035.0}},
        {{8.0, 6.0}, {1020.0, 590.0}},
        {{24.0, 18.0}, {2940.0, 1610.0}},
    };
    p2::HomographyFitResult fit;
    if (!require(p2::fit_cross_spectral_homography(
                     correspondences, &fit, &error),
                 "normalized homography fit succeeds") ||
        !require(fit.max_reprojection_error_px < 1.0e-8 &&
                     fit.root_mean_square_error_px < 1.0e-8,
                 "exact correspondences have negligible reprojection error") ||
        !require(near(fit.thermal_to_visible[0], 120.0, 1.0e-8) &&
                     near(fit.thermal_to_visible[4], 85.0, 1.0e-8) &&
                     near(fit.thermal_to_visible[2], 60.0, 1.0e-8) &&
                     near(fit.thermal_to_visible[5], 80.0, 1.0e-8),
                 "fit recovers the known affine homography"))
        return EXIT_FAILURE;

    calibration.thermal_to_visible = fit.thermal_to_visible;
    calibration.max_reprojection_error_px =
        fit.max_reprojection_error_px;
    std::ostringstream serialized;
    if (!require(p2::write_cross_spectral_calibration(
                     serialized, calibration, &error),
                 "validated calibration serializes"))
        return EXIT_FAILURE;
    p2::CrossSpectralCalibration roundtrip;
    std::istringstream serialized_input(serialized.str());
    if (!require(p2::parse_cross_spectral_calibration(
                     serialized_input, &roundtrip, &error),
                 "serialized calibration parses") ||
        !require(p2::validate_cross_spectral_calibration(
                     roundtrip, &error),
                 "serialized calibration remains usable"))
        return EXIT_FAILURE;

    const std::vector<p2::CrossSpectralCorrespondence> collinear{
        {{0.0, 0.0}, {10.0, 10.0}},
        {{1.0, 1.0}, {20.0, 20.0}},
        {{2.0, 2.0}, {30.0, 30.0}},
        {{3.0, 3.0}, {40.0, 40.0}},
    };
    if (!require(!p2::fit_cross_spectral_homography(
                     collinear, &fit, &error),
                 "collinear correspondences are rejected"))
        return EXIT_FAILURE;

    std::vector<p2::CrossSpectralCorrespondence> projective;
    const std::vector<p2::Point2d> projective_sources{
        {0.0, 0.0}, {31.0, 0.0}, {31.0, 23.0}, {0.0, 23.0},
        {4.0, 7.0}, {12.0, 19.0}, {20.0, 5.0}, {27.0, 16.0},
    };
    for (const p2::Point2d &source : projective_sources) {
        const double denominator =
            0.001 * source.x + 0.0005 * source.y + 1.0;
        projective.push_back({
            source,
            {(100.0 * source.x + 2.0 * source.y + 50.0) /
                 denominator,
             (source.x + 80.0 * source.y + 40.0) / denominator},
        });
    }
    if (!require(p2::fit_cross_spectral_homography(
                     projective, &fit, &error),
                 "projective homography fit succeeds") ||
        !require(fit.max_reprojection_error_px < 1.0e-8,
                 "projective fit reproduces exact points"))
        return EXIT_FAILURE;

    std::vector<p2::CrossSpectralCorrespondence> noisy = correspondences;
    noisy.back().visible.x += 100.0;
    if (!require(p2::fit_cross_spectral_homography(noisy, &fit, &error),
                 "noisy correspondences still produce diagnostics") ||
        !require(fit.max_reprojection_error_px > 10.0,
                 "outlier is visible in maximum reprojection error"))
        return EXIT_FAILURE;

    std::cout << "cross-spectral calibration tests passed\n";
    return EXIT_SUCCESS;
}
