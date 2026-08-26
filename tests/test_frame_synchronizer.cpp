#include "p2/sync/frame_synchronizer.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

p2::VisibleFrameEvent visible(std::uint32_t sequence,
                              std::uint64_t timestamp_ns)
{
    p2::VisibleFrameEvent frame;
    frame.sequence = sequence;
    frame.timestamp_ns = timestamp_ns;
    frame.arrival_ns = timestamp_ns + 1;
    return frame;
}

p2::ThermalFrameEvent thermal(std::uint32_t sequence,
                              std::uint64_t timestamp_ns)
{
    p2::ThermalFrameEvent frame;
    frame.sequence = sequence;
    frame.pair_sequence = sequence;
    frame.timestamp_ns = timestamp_ns;
    frame.first_ready_ns = timestamp_ns - 10;
    frame.second_ready_ns = timestamp_ns + 10;
    frame.arrival_ns = timestamp_ns + 20;
    return frame;
}

void test_nearest_visible_is_selected()
{
    p2::SynchronizerConfig config;
    config.lookahead_ns = 0;
    config.max_skew_ns = 100;
    p2::FrameSynchronizer synchronizer(config);
    synchronizer.push_visible(visible(1, 100));
    synchronizer.push_visible(visible(2, 133));
    synchronizer.push_thermal(thermal(7, 120));
    synchronizer.flush();

    const auto matches = synchronizer.matches();
    require(matches.size() == 1, "expected one match");
    require(matches[0].visible_sequence == 2,
            "nearest visible frame was not selected");
    require(matches[0].signed_delta_ns == 13,
            "signed delta has the wrong sign or value");
}

void test_skew_gate_rejects_distant_frame()
{
    p2::SynchronizerConfig config;
    config.lookahead_ns = 0;
    config.max_skew_ns = 5;
    p2::FrameSynchronizer synchronizer(config);
    synchronizer.push_visible(visible(1, 100));
    synchronizer.push_thermal(thermal(1, 120));
    synchronizer.flush();

    const auto stats = synchronizer.stats();
    require(stats.matched == 0, "distant frame unexpectedly matched");
    require(stats.unmatched_skew == 1, "skew rejection was not counted");
}

void test_thermal_waits_for_visible_lookahead()
{
    p2::SynchronizerConfig config;
    config.lookahead_ns = 20;
    config.max_skew_ns = 100;
    p2::FrameSynchronizer synchronizer(config);
    synchronizer.push_thermal(thermal(1, 100));
    synchronizer.push_visible(visible(1, 90));
    require(synchronizer.matches().empty(),
            "thermal frame matched before lookahead was available");
    synchronizer.push_visible(visible(2, 125));

    const auto matches = synchronizer.matches();
    require(matches.size() == 1, "pending thermal frame did not match");
    require(matches[0].visible_sequence == 1,
            "lookahead changed the nearest-neighbor result");
}

void test_bounded_thermal_queue_counts_drop()
{
    p2::SynchronizerConfig config;
    config.max_pending_thermal = 2;
    config.lookahead_ns = 1000;
    p2::FrameSynchronizer synchronizer(config);
    synchronizer.push_thermal(thermal(1, 100));
    synchronizer.push_thermal(thermal(2, 200));
    synchronizer.push_thermal(thermal(3, 300));

    const auto stats = synchronizer.stats();
    require(stats.thermal_queue_drops == 1,
            "thermal queue overflow was not counted");
    require(stats.thermal_pending == 2,
            "thermal queue exceeded its configured bound");
}

void test_out_of_order_visible_is_sorted()
{
    p2::SynchronizerConfig config;
    config.lookahead_ns = 0;
    config.max_skew_ns = 100;
    p2::FrameSynchronizer synchronizer(config);
    synchronizer.push_visible(visible(2, 130));
    synchronizer.push_visible(visible(1, 100));
    synchronizer.push_thermal(thermal(1, 105));

    const auto matches = synchronizer.matches();
    const auto stats = synchronizer.stats();
    require(matches.size() == 1 && matches[0].visible_sequence == 1,
            "out-of-order visible event was not sorted");
    require(stats.visible_out_of_order == 1,
            "out-of-order visible event was not counted");
}

}  // namespace

int main()
{
    try {
        test_nearest_visible_is_selected();
        test_skew_gate_rejects_distant_frame();
        test_thermal_waits_for_visible_lookahead();
        test_bounded_thermal_queue_counts_drop();
        test_out_of_order_visible_is_sorted();
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "FRAME_SYNCHRONIZER_TEST=PASS\n";
    return EXIT_SUCCESS;
}
