#include "p2/capture/device_discovery.hpp"
#include "p2/capture/v4l2_utils.hpp"
#include "p2/capture/visible_capture.hpp"
#include "p2/pipeline/bounded_queue.hpp"
#include "p2/streaming/mpp_h264_encoder.hpp"
#include "p2/streaming/rtsp_publisher.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

std::atomic<bool> g_stop{false};

struct Options {
    std::string visible;
    std::string rtsp_url = "rtsp://127.0.0.1:8554/p2";
    std::string h264_output;
    std::string summary_json = "/tmp/p2-mpp-stream-summary.json";
    std::uint64_t duration_seconds = 30;
    std::uint32_t bitrate_bps = 6'000'000;
    bool demo_overlay = false;
};

void signal_handler(int)
{
    g_stop.store(true);
}

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --visible PATH       override RKISP mainpath\n"
        << "  --rtsp-url URL       MediaMTX publish URL; 'none' disables\n"
        << "  --h264-output FILE   optional Annex-B evidence file\n"
        << "  --duration-sec N     run duration, default 30\n"
        << "  --bitrate-bps N      target bitrate, default 6000000\n"
        << "  --demo-overlay       draw a synthetic box for OSD validation\n"
        << "  --summary FILE       summary JSON output\n"
        << "  --help               show this text\n";
}

bool parse_u64(const char *text, std::uint64_t *value)
{
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *value = parsed;
    return true;
}

bool parse_options(int argc, char **argv, Options *options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (argument == "--demo-overlay") {
            options->demo_overlay = true;
            continue;
        }
        if (index + 1 >= argc)
            return false;
        const char *value = argv[++index];
        if (argument == "--visible")
            options->visible = value;
        else if (argument == "--rtsp-url")
            options->rtsp_url = value;
        else if (argument == "--h264-output")
            options->h264_output = value;
        else if (argument == "--summary")
            options->summary_json = value;
        else if (argument == "--duration-sec") {
            if (!parse_u64(value, &options->duration_seconds))
                return false;
        } else if (argument == "--bitrate-bps") {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, &parsed) || parsed == 0U ||
                parsed > UINT32_MAX)
                return false;
            options->bitrate_bps = static_cast<std::uint32_t>(parsed);
        } else {
            return false;
        }
    }
    return options->duration_seconds != 0U;
}

void write_summary(std::ostream &output, const Options &options,
                   const p2::VisibleCaptureStats &capture,
                   const p2::BoundedQueueStats &queue,
                   const p2::MppH264EncoderStats &encoder,
                   const p2::RtspPublisherStats &publisher,
                   double elapsed_seconds, bool passed,
                   const std::string &error)
{
    const double frames = static_cast<double>(encoder.encoded_frames);
    output << std::fixed << std::setprecision(3)
           << "{\n"
           << "  \"schema\": \"p2.mpp-stream.v1\",\n"
           << "  \"result\": \"" << (passed ? "PASS" : "FAIL")
           << "\",\n"
           << "  \"error\": \"" << error << "\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT
           << "\",\n"
           << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n"
           << "  \"visible_device\": \"" << options.visible << "\",\n"
           << "  \"rtsp_url\": \"" << options.rtsp_url << "\",\n"
           << "  \"output\": {\"width\": 1080, \"height\": 1920, "
              "\"fps\": 30, \"bitrate_bps\": "
           << options.bitrate_bps << "},\n"
           << "  \"capture\": {\"dqbuf\": " << capture.dqbuf_count
           << ", \"sequence_gaps\": " << capture.sequence_gaps
           << ", \"bad_bytes_used\": " << capture.bad_bytes_used
           << ", \"missing_timestamp\": "
           << capture.missing_monotonic_timestamp
           << ", \"lease_high_watermark\": "
           << capture.lease_high_watermark << "},\n"
           << "  \"queue\": {\"pushed\": " << queue.pushed
           << ", \"popped\": " << queue.popped
           << ", \"drop_oldest\": " << queue.dropped_oldest
           << ", \"high_watermark\": " << queue.high_watermark << "},\n"
           << "  \"encoder\": {\"frames\": "
           << encoder.encoded_frames << ", \"bytes\": "
           << encoder.encoded_bytes << ", \"key_frames\": "
           << encoder.key_frames << ", \"source_dma_buf_imports\": "
           << encoder.source_dma_buf_imports << ", \"osd_frames\": "
           << encoder.osd_frames << ", \"average_rga_ms\": "
           << (frames == 0.0 ? 0.0 : encoder.rga_total_ms / frames)
           << ", \"average_mpp_ms\": "
           << (frames == 0.0 ? 0.0 : encoder.mpp_total_ms / frames)
           << "},\n"
           << "  \"publisher\": {\"packets\": " << publisher.packets
           << ", \"bytes\": " << publisher.bytes
           << ", \"failures\": " << publisher.failures << "}\n"
           << "}\n";
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    p2::DiscoveredDevices devices;
    std::string error;
    if (options.visible.empty()) {
        if (!p2::discover_devices(&devices, &error)) {
            std::cerr << "device discovery failed: " << error << '\n';
            return EXIT_FAILURE;
        }
        options.visible = devices.visible;
    }

    p2::MppH264EncoderConfig encoder_config;
    encoder_config.bitrate_bps = options.bitrate_bps;
    p2::MppH264Encoder encoder(encoder_config);
    if (!encoder.initialize(&error)) {
        std::cerr << "encoder initialization failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    const bool publish_enabled = options.rtsp_url != "none";
    p2::RtspPublisherConfig publisher_config;
    publisher_config.url = options.rtsp_url;
    p2::RtspPublisher publisher(publisher_config);
    if (publish_enabled &&
        !publisher.connect(encoder.codec_header(), &error)) {
        std::cerr << "RTSP publisher connection failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    std::ofstream h264;
    if (!options.h264_output.empty()) {
        h264.open(options.h264_output, std::ios::binary);
        if (!h264) {
            std::cerr << "failed to open H.264 output "
                      << options.h264_output << '\n';
            return EXIT_FAILURE;
        }
        const std::vector<std::uint8_t> &header = encoder.codec_header();
        h264.write(reinterpret_cast<const char *>(header.data()),
                   static_cast<std::streamsize>(header.size()));
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    p2::BoundedLatestQueue<p2::VisibleFrameLease> queue(1);
    p2::VisibleCapture capture({
        options.visible, 3840, 2160, 6, 500, true,
    });
    const std::uint64_t started_ns = p2::monotonic_now_ns();
    const std::uint64_t deadline_ns = started_ns +
        options.duration_seconds * 1'000'000'000ULL;
    bool capture_ok = false;
    std::string capture_error;
    std::mutex failure_mutex;
    std::string failure;
    const auto set_failure = [&](const std::string &message) {
        {
            std::lock_guard<std::mutex> lock(failure_mutex);
            if (failure.empty())
                failure = message;
        }
        g_stop.store(true);
    };
    std::thread capture_thread([&]() {
        capture_ok = capture.run_leased_frames(
            g_stop,
            [&](p2::VisibleFrameLease &&lease) {
                if (p2::monotonic_now_ns() >= deadline_ns) {
                    g_stop.store(true);
                    return;
                }
                if (!queue.push(std::move(lease))) {
                    set_failure("encoder queue closed during capture");
                }
            },
            &capture_error);
        queue.close(false);
    });

    p2::VideoOverlay overlay;
    overlay.banner = "P2 MONITOR";
    overlay.banner_color = p2::OverlayColor::green;
    if (options.demo_overlay) {
        overlay.banner = "P2 ALERT";
        overlay.banner_color = p2::OverlayColor::red;
        overlay.boxes.push_back({
            120, 320, 760, 1500, p2::OverlayColor::red,
            "PERSON 99% 36.5C",
        });
    }
    p2::VisibleFrameLease lease;
    while (queue.wait_pop(&lease)) {
        const p2::VisibleFrameView &frame = lease.view();
        p2::EncodedH264Packet packet;
        if (frame.data_offset != 0U ||
            !encoder.encode_nv12_dmabuf(
                frame.dma_buf_fd, frame.size, frame.width, frame.height,
                frame.bytes_per_line, frame.event.timestamp_ns,
                overlay, &packet, &error)) {
            set_failure(error);
            lease.reset();
            break;
        }
        lease.reset();
        if (h264)
            h264.write(reinterpret_cast<const char *>(packet.bytes.data()),
                       static_cast<std::streamsize>(packet.bytes.size()));
        if (publish_enabled && !publisher.publish(packet, &error)) {
            set_failure(error);
            break;
        }
    }
    g_stop.store(true);
    capture_thread.join();
    publisher.close();
    if (!capture_ok)
        set_failure("visible capture failed: " + capture_error);
    {
        std::lock_guard<std::mutex> lock(failure_mutex);
        error = failure;
    }
    const double elapsed_seconds = static_cast<double>(
        p2::monotonic_now_ns() - started_ns) / 1.0e9;
    const p2::VisibleCaptureStats capture_stats = capture.stats();
    const p2::MppH264EncoderStats encoder_stats = encoder.stats();
    const p2::RtspPublisherStats publisher_stats = publisher.stats();
    const bool passed = error.empty() && capture_ok &&
        encoder_stats.encoded_frames > 0U &&
        capture_stats.bad_bytes_used == 0U &&
        capture_stats.missing_monotonic_timestamp == 0U &&
        (!publish_enabled ||
         (publisher_stats.packets == encoder_stats.encoded_frames &&
          publisher_stats.failures == 0U));
    write_summary(std::cout, options, capture_stats, queue.stats(),
                  encoder_stats, publisher_stats, elapsed_seconds,
                  passed, error);
    std::ofstream summary(options.summary_json);
    if (!summary) {
        std::cerr << "failed to open summary JSON "
                  << options.summary_json << '\n';
        return EXIT_FAILURE;
    }
    write_summary(summary, options, capture_stats, queue.stats(),
                  encoder_stats, publisher_stats, elapsed_seconds,
                  passed, error);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
