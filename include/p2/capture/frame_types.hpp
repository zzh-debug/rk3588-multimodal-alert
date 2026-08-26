#pragma once

#include <cstdint>

namespace p2 {

struct VisibleFrameEvent {
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t arrival_ns = 0;
    std::uint32_t bytes_used = 0;
    std::uint32_t flags = 0;
};

struct ThermalFrameEvent {
    std::uint32_t sequence = 0;
    std::uint32_t pair_sequence = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t first_ready_ns = 0;
    std::uint64_t second_ready_ns = 0;
    std::uint64_t second_read_done_ns = 0;
    std::uint64_t arrival_ns = 0;
    std::uint32_t flags = 0;
};

struct MatchRecord {
    std::uint32_t visible_sequence = 0;
    std::uint32_t thermal_sequence = 0;
    std::uint32_t thermal_pair_sequence = 0;
    std::uint64_t visible_timestamp_ns = 0;
    std::uint64_t thermal_timestamp_ns = 0;
    std::uint64_t thermal_span_ns = 0;
    std::uint64_t visible_arrival_ns = 0;
    std::uint64_t thermal_arrival_ns = 0;
    std::int64_t signed_delta_ns = 0;
    std::uint64_t absolute_delta_ns = 0;
};

}  // namespace p2
