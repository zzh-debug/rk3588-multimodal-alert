#pragma once

#include "p2/streaming/mpp_h264_encoder.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace p2 {

struct RtspPublisherConfig {
    std::string url = "rtsp://127.0.0.1:8554/p2";
    std::uint32_t width = 1080;
    std::uint32_t height = 1920;
    std::uint32_t fps = 30;
    bool tcp_transport = true;
};

struct RtspPublisherStats {
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
    std::uint64_t failures = 0;
};

class RtspPublisher {
public:
    explicit RtspPublisher(RtspPublisherConfig config = {});
    ~RtspPublisher();

    RtspPublisher(const RtspPublisher &) = delete;
    RtspPublisher &operator=(const RtspPublisher &) = delete;

    bool connect(const std::vector<std::uint8_t> &annex_b_header,
                 std::string *error);
    bool publish(const EncodedH264Packet &packet, std::string *error);
    void close();
    bool connected() const;
    const RtspPublisherStats &stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace p2
