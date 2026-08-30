#pragma once

#include "p2/inference/letterbox.hpp"
#include "p2/streaming/video_overlay.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace p2 {

struct MppH264EncoderConfig {
    std::uint32_t width = 1080;
    std::uint32_t height = 1920;
    std::uint32_t fps = 30;
    std::uint32_t bitrate_bps = 6'000'000;
    std::uint32_t gop_frames = 60;
    ImageRotation rotation = ImageRotation::kClockwise270;
};

struct EncodedH264Packet {
    std::vector<std::uint8_t> bytes;
    std::int64_t pts = 0;
    bool key_frame = false;
};

struct MppH264EncoderStats {
    std::uint64_t encoded_frames = 0;
    std::uint64_t encoded_bytes = 0;
    std::uint64_t key_frames = 0;
    std::uint64_t source_dma_buf_imports = 0;
    std::uint64_t osd_frames = 0;
    double rga_total_ms = 0.0;
    double mpp_total_ms = 0.0;
};

class MppH264Encoder {
public:
    explicit MppH264Encoder(MppH264EncoderConfig config = {});
    ~MppH264Encoder();

    MppH264Encoder(const MppH264Encoder &) = delete;
    MppH264Encoder &operator=(const MppH264Encoder &) = delete;

    bool initialize(std::string *error);
    bool encode_nv12_dmabuf(int dma_buf_fd,
                            std::size_t size,
                            std::uint32_t source_width,
                            std::uint32_t source_height,
                            std::uint32_t source_stride,
                            std::uint64_t capture_timestamp_ns,
                            const VideoOverlay &overlay,
                            EncodedH264Packet *packet,
                            std::string *error);

    const std::vector<std::uint8_t> &codec_header() const;
    const MppH264EncoderStats &stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace p2
