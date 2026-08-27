#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace p2 {

inline constexpr std::size_t kZmlxMetaV1Bytes = 3400;

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

// Exact project-one ZMLX v1 payload plus the decoded timing event. Keeping the
// bytes intact makes captures auditable and lets the temperature layer decode
// endianness independently from the V4L2 buffer lifetime.
struct ThermalFramePayload {
    ThermalFrameEvent event;
    std::array<std::uint8_t, kZmlxMetaV1Bytes> zmlx_bytes{};
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
