#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "p2/capture/frame_types.hpp"

namespace p2 {

struct ThermalCaptureConfig {
    std::string device;
    std::uint32_t requested_buffers = 4;
    int poll_timeout_ms = 1000;
};

struct ThermalCaptureStats {
    std::uint64_t dqbuf_count = 0;
    std::uint64_t valid_pairs = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t invalid_payloads = 0;
    std::uint64_t missing_monotonic_timestamp = 0;
    std::uint64_t timestamp_mismatches = 0;
    std::uint64_t poll_timeouts = 0;
    std::uint32_t first_sequence = 0;
    std::uint32_t last_sequence = 0;
    std::uint32_t buffer_size = 0;
    std::uint32_t allocated_buffers = 0;
};

class ThermalCapture {
public:
    explicit ThermalCapture(ThermalCaptureConfig config);

    bool run(const std::atomic<bool> &stop,
             const std::function<void(const ThermalFrameEvent &)> &on_frame,
             std::string *error);
    const ThermalCaptureStats &stats() const { return stats_; }

private:
    ThermalCaptureConfig config_;
    ThermalCaptureStats stats_;
};

}  // namespace p2
