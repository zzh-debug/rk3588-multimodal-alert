#include "p2/fusion/multimodal_fusion.hpp"
#include "p2/fusion/temporal_joiner.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

p2::CrossSpectralCalibration calibration()
{
    p2::CrossSpectralCalibration value;
    value.schema = p2::kCrossSpectralCalibrationSchema;
    value.state = p2::CalibrationState::calibrated;
    value.calibration_id = "fusion-synthetic";
    value.thermal_model = p2::Mlx90640Model::esf_baa;
    value.thermal_width = 32;
    value.thermal_height = 24;
    value.visible_width = 3840;
    value.visible_height = 2160;
    value.nominal_distance_mm = 1000.0;
    value.valid_distance_min_mm = 800.0;
    value.valid_distance_max_mm = 1200.0;
    value.thermal_to_visible = {
        100.0, 0.0, 0.0,
        0.0, 80.0, 0.0,
        0.0, 0.0, 1.0,
    };
    value.max_reprojection_error_px = 20.0;
    return value;
}

void test_hot_component_maps_and_matches_person()
{
    std::array<float, p2::kMlx90640Pixels> temperatures;
    temperatures.fill(25.0F);
    temperatures[8U * 32U + 10U] = 35.0F;
    temperatures[8U * 32U + 11U] = 36.0F;
    temperatures[9U * 32U + 10U] = 34.0F;
    temperatures[9U * 32U + 11U] = 35.0F;
    temperatures[0] = 40.0F;  // isolated noise must be rejected

    std::vector<p2::PersonDetection> people{
        {{900.0F, 500.0F, 1300.0F, 1000.0F}, 0.90F},
        {{2000.0F, 1000.0F, 2500.0F, 1900.0F}, 0.95F},
    };
    p2::MultimodalFusionConfig config;
    config.mapping_margin_px = 40.0;
    config.minimum_thermal_overlap = 0.5;
    p2::MultimodalFusionResult result;
    std::string error;
    require(p2::analyze_multimodal_frame(
                calibration(), temperatures, people, config, &result,
                &error),
            "fusion failed: " + error);
    require(std::fabs(result.background_temperature_c - 25.0F) < 1.0e-6F,
            "median background is wrong");
    require(std::fabs(result.activation_temperature_c - 30.0F) < 1.0e-6F,
            "absolute/relative threshold selection is wrong");
    require(result.hot_regions.size() == 1,
            "connected component or minimum size gate is wrong");
    const p2::ThermalHotRegion &region = result.hot_regions[0];
    require(region.thermal_bounds.first_column == 10 &&
                region.thermal_bounds.last_column == 11 &&
                region.thermal_bounds.first_row == 8 &&
                region.thermal_bounds.last_row == 9,
            "thermal component bounds are wrong");
    require(region.expanded_visible_bounds.x_min == 910.0 &&
                region.expanded_visible_bounds.x_max == 1190.0 &&
                region.expanded_visible_bounds.y_min == 560.0 &&
                region.expanded_visible_bounds.y_max == 800.0,
            "mapping uncertainty expansion is wrong");
    require(result.matches.size() == 1 &&
                result.matches[0].person_index == 0 &&
                result.matches[0].thermal_overlap > 0.99,
            "person/thermal region association is wrong");
    require(result.person_with_thermal_evidence,
            "positive fusion evidence was not raised");
}

void test_finite_pixel_gate_and_no_match()
{
    std::array<float, p2::kMlx90640Pixels> temperatures;
    temperatures.fill(25.0F);
    temperatures[5] = std::numeric_limits<float>::quiet_NaN();
    p2::MultimodalFusionConfig config;
    config.minimum_finite_pixels = p2::kMlx90640Pixels;
    p2::MultimodalFusionResult result;
    std::string error;
    require(!p2::analyze_multimodal_frame(
                calibration(), temperatures, {}, config, &result, &error),
            "finite pixel gate accepted an incomplete matrix");

    temperatures[5] = 25.0F;
    require(p2::analyze_multimodal_frame(
                calibration(), temperatures, {}, config, &result, &error),
            "all-finite background frame failed");
    require(result.hot_regions.empty() && result.matches.empty() &&
                !result.person_with_thermal_evidence,
            "background-only frame produced fusion evidence");
}

void test_alert_debounce_and_cooldown()
{
    p2::AlertDebounceConfig config;
    config.confirmation_frames = 2;
    config.release_frames = 3;
    config.cooldown_ns = 100;
    p2::AlertDebouncer debouncer(config);
    p2::AlertUpdate update;
    std::string error;
    require(debouncer.update(10, true, &update, &error) &&
                update.state == p2::AlertState::candidate && !update.active,
            "first evidence did not enter candidate state");
    require(debouncer.update(20, true, &update, &error) && update.active &&
                update.transition == p2::AlertTransition::activated,
            "confirmation did not activate alert");
    require(debouncer.update(30, false, &update, &error) && update.active,
            "first miss released alert too early");
    require(debouncer.update(40, false, &update, &error) && update.active,
            "second miss released alert too early");
    require(debouncer.update(50, false, &update, &error) && !update.active &&
                update.state == p2::AlertState::cooldown &&
                update.transition == p2::AlertTransition::deactivated,
            "release hysteresis did not enter cooldown");
    require(debouncer.update(100, true, &update, &error) &&
                update.state == p2::AlertState::cooldown,
            "cooldown accepted early evidence");
    require(debouncer.update(150, true, &update, &error) &&
                update.state == p2::AlertState::candidate,
            "evidence after cooldown did not restart confirmation");
    require(!debouncer.update(149, true, &update, &error),
            "backwards alert timestamp was accepted");
}

p2::VisibleFusionFrame visible(std::uint32_t sequence,
                               std::uint64_t timestamp)
{
    p2::VisibleFusionFrame frame;
    frame.event.sequence = sequence;
    frame.event.timestamp_ns = timestamp;
    frame.detections.push_back({{1.0F, 2.0F, 3.0F, 4.0F}, 0.8F});
    return frame;
}

p2::ThermalFusionFrame thermal(std::uint32_t sequence,
                               std::uint64_t timestamp)
{
    p2::ThermalFusionFrame frame;
    frame.event.sequence = sequence;
    frame.event.pair_sequence = sequence + 100;
    frame.event.timestamp_ns = timestamp;
    frame.event.first_ready_ns = timestamp - 5;
    frame.event.second_ready_ns = timestamp + 5;
    frame.temperature_c.fill(27.0F);
    return frame;
}

void test_temporal_join_preserves_results_and_is_bounded()
{
    p2::TemporalFusionJoinerConfig config;
    config.synchronizer.lookahead_ns = 20;
    config.synchronizer.max_skew_ns = 100;
    config.max_visible_results = 4;
    config.max_thermal_results = 2;
    p2::TemporalFusionJoiner joiner(config);

    require(joiner.push_thermal(thermal(7, 100)).empty(),
            "thermal frame matched without visible lookahead");
    require(joiner.push_visible(visible(1, 90)).empty(),
            "visible frame before lookahead completed pairing");
    const auto pairs = joiner.push_visible(visible(2, 125));
    require(pairs.size() == 1 && pairs[0].match.visible_sequence == 1 &&
                pairs[0].thermal.event.sequence == 7 &&
                pairs[0].visible.detections.size() == 1 &&
                pairs[0].thermal.temperature_c[0] == 27.0F,
            "temporal join lost the nearest frame payload");
    require(joiner.stats().emitted_pairs == 1,
            "temporal join emitted-pair statistic is wrong");

    p2::TemporalFusionJoinerConfig bounded_config;
    bounded_config.synchronizer.lookahead_ns = 1000;
    bounded_config.synchronizer.max_skew_ns = 5000;
    bounded_config.synchronizer.max_pending_thermal = 4;
    bounded_config.max_visible_results = 2;
    bounded_config.max_thermal_results = 1;
    p2::TemporalFusionJoiner bounded(bounded_config);
    bounded.push_thermal(thermal(1, 100));
    bounded.push_thermal(thermal(2, 200));
    const auto joined = bounded.push_visible(visible(3, 2000));
    require(bounded.flush().empty(),
            "flush repeated an already emitted bounded match");
    const auto stats = bounded.stats();
    require(joined.size() == 1 && joined[0].thermal.event.sequence == 2,
            "bounded join did not retain the newest thermal result");
    require(stats.thermal_result_drops == 1 &&
                stats.missing_thermal_results == 1,
            "bounded join did not account for an evicted result");
}

}  // namespace

int main()
{
    try {
        test_hot_component_maps_and_matches_person();
        test_finite_pixel_gate_and_no_match();
        test_alert_debounce_and_cooldown();
        test_temporal_join_preserves_results_and_is_bounded();
    } catch (const std::exception &exception) {
        std::cerr << "multimodal fusion test failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "multimodal fusion tests passed\n";
    return EXIT_SUCCESS;
}
