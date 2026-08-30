#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "p2/capture/frame_types.hpp"

namespace p2 {

class VisibleCaptureSession;

struct VisibleCaptureConfig {
    std::string device;
    std::uint32_t width = 3840;
    std::uint32_t height = 2160;
    std::uint32_t requested_buffers = 6;
    int poll_timeout_ms = 500;
    bool export_dma_buf = false;
};

struct VisibleCaptureStats {
    std::uint64_t dqbuf_count = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t bad_bytes_used = 0;
    std::uint64_t missing_monotonic_timestamp = 0;
    std::uint64_t poll_timeouts = 0;
    std::uint32_t first_sequence = 0;
    std::uint32_t last_sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bytes_per_line = 0;
    std::uint32_t size_image = 0;
    std::uint32_t allocated_buffers = 0;
    std::uint32_t lease_high_watermark = 0;
    std::uint32_t exported_dma_buffers = 0;
};

class VisibleFrameLease {
public:
    VisibleFrameLease() = default;
    ~VisibleFrameLease();

    VisibleFrameLease(VisibleFrameLease &&other) noexcept;
    VisibleFrameLease &operator=(VisibleFrameLease &&other) noexcept;
    VisibleFrameLease(const VisibleFrameLease &) = delete;
    VisibleFrameLease &operator=(const VisibleFrameLease &) = delete;

    explicit operator bool() const { return session_ != nullptr; }
    const VisibleFrameView &view() const { return view_; }
    void reset();

private:
    friend class VisibleCapture;
    VisibleFrameLease(VisibleFrameView view,
                      std::uint32_t buffer_index,
                      std::shared_ptr<VisibleCaptureSession> session);

    VisibleFrameView view_{};
    std::uint32_t buffer_index_ = 0;
    std::shared_ptr<VisibleCaptureSession> session_;
};

class VisibleCapture {
public:
    explicit VisibleCapture(VisibleCaptureConfig config);

    bool run(const std::atomic<bool> &stop,
             const std::function<void(const VisibleFrameEvent &)> &on_frame,
             std::string *error);
    bool run_frames(
        const std::atomic<bool> &stop,
        const std::function<void(const VisibleFrameView &)> &on_frame,
        std::string *error);
    bool run_leased_frames(
        const std::atomic<bool> &stop,
        const std::function<void(VisibleFrameLease &&)> &on_frame,
        std::string *error);
    const VisibleCaptureStats &stats() const { return stats_; }

private:
    VisibleCaptureConfig config_;
    VisibleCaptureStats stats_;
};

}  // namespace p2
