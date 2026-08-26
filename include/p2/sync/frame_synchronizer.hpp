#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "p2/capture/frame_types.hpp"

namespace p2 {

struct SynchronizerConfig {
    std::size_t max_visible_history = 64;
    std::size_t max_pending_thermal = 16;
    std::uint64_t lookahead_ns = 20'000'000ULL;
    std::uint64_t max_skew_ns = 50'000'000ULL;
};

struct SynchronizerStats {
    std::uint64_t visible_received = 0;
    std::uint64_t thermal_received = 0;
    std::uint64_t matched = 0;
    std::uint64_t unmatched_skew = 0;
    std::uint64_t unmatched_no_visible = 0;
    std::uint64_t thermal_queue_drops = 0;
    std::uint64_t visible_history_pruned = 0;
    std::uint64_t visible_out_of_order = 0;
    std::uint64_t thermal_out_of_order = 0;
    std::size_t visible_high_watermark = 0;
    std::size_t thermal_high_watermark = 0;
    std::size_t visible_pending = 0;
    std::size_t thermal_pending = 0;
};

class FrameSynchronizer {
public:
    explicit FrameSynchronizer(SynchronizerConfig config = {});

    void push_visible(const VisibleFrameEvent &frame);
    void push_thermal(const ThermalFrameEvent &frame);
    void flush();

    SynchronizerStats stats() const;
    std::vector<MatchRecord> matches() const;

private:
    void process_pending_locked(bool force);
    void prune_visible_locked();

    SynchronizerConfig config_;
    mutable std::mutex mutex_;
    std::deque<VisibleFrameEvent> visible_;
    std::deque<ThermalFrameEvent> thermal_;
    SynchronizerStats stats_;
    std::vector<MatchRecord> matches_;
};

}  // namespace p2
