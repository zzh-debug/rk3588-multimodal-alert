#include "p2/calibration/cross_spectral.hpp"
#include "p2/capture/device_discovery.hpp"
#include "p2/capture/thermal_capture.hpp"
#include "p2/capture/v4l2_utils.hpp"
#include "p2/capture/visible_capture.hpp"
#include "p2/fusion/multimodal_fusion.hpp"
#include "p2/fusion/temporal_joiner.hpp"
#include "p2/inference/person_detector.hpp"
#include "p2/pipeline/bounded_queue.hpp"
#include "p2/streaming/mpp_h264_encoder.hpp"
#include "p2/streaming/rtsp_publisher.hpp"
#include "p2/streaming/video_overlay.hpp"
#include "p2/thermal/mlx90640_math.hpp"
#include "p2/thermal/temporal_filter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

constexpr std::uint32_t kCaptureBufferCount = 6;
constexpr const char *kModelSha256 =
    "7c6b801de602b8aaa72269fab8daab21810418a4b01c8e8995313916e278fe71";

std::atomic<bool> g_stop{false};

struct Options {
    std::string visible;
    std::string thermal;
    std::string eeprom =
        "/sys/bus/nvmem/devices/zzh_mlx90640_eeprom-5-33/nvmem";
    std::string model = "/usr/share/p2/models/yolov5s-640-640.rknn";
    std::string calibration = "/etc/p2/cross_spectral_calibration.conf";
    std::string summary_json = "/tmp/p2-fusion-summary.json";
    std::string event_csv = "/tmp/p2-fusion-events.csv";
    std::string rtsp_url;
    std::string h264_output;
    std::uint64_t duration_seconds = 30;
    std::uint64_t maximum_pairs = 0;
    p2::ImageRotation rotation = p2::ImageRotation::kClockwise270;
    p2::MultimodalFusionConfig fusion;
    p2::AlertDebounceConfig debounce;
    float confidence_threshold = 0.25F;
    float nms_threshold = 0.45F;
    float emissivity = 0.95F;
    float reflected_offset_c = -8.0F;
    float filter_tau_ms = 250.0F;
    std::uint64_t filter_reset_gap_ms = 1000;
    std::uint64_t lookahead_ms = 20;
    std::uint64_t maximum_skew_ms = 50;
    std::uint64_t overlay_ttl_ms = 1000;
    std::uint32_t stream_bitrate_bps = 6'000'000;
};

struct InferenceStats {
    std::uint64_t frames = 0;
    std::uint64_t failures = 0;
    std::uint64_t person_detections = 0;
    std::vector<double> total_ms;
};

struct ThermalProcessingStats {
    std::uint64_t frames = 0;
    std::uint64_t math_failures = 0;
    std::uint64_t filter_failures = 0;
    std::uint64_t repaired_nonfinite_temperature_pixels = 0;
    std::uint64_t repaired_nonfinite_image_pixels = 0;
};

struct FusionStats {
    std::uint64_t pairs = 0;
    std::uint64_t failures = 0;
    std::uint64_t evidence_frames = 0;
    std::uint64_t active_frames = 0;
    std::uint64_t activations = 0;
    std::uint64_t deactivations = 0;
    std::uint64_t total_people = 0;
    std::uint64_t total_hot_regions = 0;
    std::uint64_t total_matches = 0;
    std::vector<double> absolute_skew_ms;
    std::vector<double> fusion_ms;
};

struct OverlayState {
    p2::VideoOverlay overlay;
    std::uint64_t visible_timestamp_ns = 0;
};

void signal_handler(int)
{
    g_stop.store(true);
}

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --visible PATH             override RKISP mainpath\n"
        << "  --thermal PATH             override ZMLX meta node\n"
        << "  --eeprom FILE              MLX90640 EEPROM NVMEM\n"
        << "  --model FILE               RKNN YOLOv5s model\n"
        << "  --calibration FILE         cross-spectral config\n"
        << "  --duration-sec N           run duration, default 30\n"
        << "  --pairs N                  optional fused-pair limit\n"
        << "  --rotation VALUE           0, 90cw, 180, 270cw\n"
        << "  --confidence VALUE         person threshold, default 0.25\n"
        << "  --hot-absolute-c VALUE      absolute heat threshold, default 30\n"
        << "  --hot-delta-c VALUE         background delta, default 4\n"
        << "  --hot-min-pixels N          connected pixels, default 2\n"
        << "  --mapping-margin-px VALUE   mapping uncertainty, default 240\n"
        << "  --minimum-overlap VALUE     thermal ROI coverage, default 0.10\n"
        << "  --confirm-frames N          activation debounce, default 2\n"
        << "  --release-frames N          release debounce, default 3\n"
        << "  --cooldown-ms N             reactivation cooldown, default 1000\n"
        << "  --lookahead-ms N            temporal lookahead, default 20\n"
        << "  --max-skew-ms N             temporal gate, default 50\n"
        << "  --rtsp-url URL              enable MPP/OSD RTSP publishing\n"
        << "  --h264-output FILE          optional Annex-B evidence file\n"
        << "  --stream-bitrate-bps N      H.264 target, default 6000000\n"
        << "  --overlay-ttl-ms N          latest-result lifetime, default 1000\n"
        << "  --summary FILE              summary JSON output\n"
        << "  --events FILE               per-pair CSV output\n"
        << "  --help                      show this text\n";
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

bool parse_float(const char *text, float *value)
{
    char *end = nullptr;
    errno = 0;
    const float parsed = std::strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !std::isfinite(parsed))
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
        if (index + 1 >= argc)
            return false;
        const char *value = argv[++index];
        if (argument == "--visible")
            options->visible = value;
        else if (argument == "--thermal")
            options->thermal = value;
        else if (argument == "--eeprom")
            options->eeprom = value;
        else if (argument == "--model")
            options->model = value;
        else if (argument == "--calibration")
            options->calibration = value;
        else if (argument == "--summary")
            options->summary_json = value;
        else if (argument == "--events")
            options->event_csv = value;
        else if (argument == "--rtsp-url")
            options->rtsp_url = value;
        else if (argument == "--h264-output")
            options->h264_output = value;
        else if (argument == "--rotation") {
            if (!p2::parse_image_rotation(value, &options->rotation))
                return false;
        } else if (argument == "--duration-sec") {
            if (!parse_u64(value, &options->duration_seconds))
                return false;
        } else if (argument == "--pairs") {
            if (!parse_u64(value, &options->maximum_pairs))
                return false;
        } else if (argument == "--confidence") {
            if (!parse_float(value, &options->confidence_threshold))
                return false;
        } else if (argument == "--hot-absolute-c") {
            if (!parse_float(value,
                             &options->fusion.absolute_hot_temperature_c))
                return false;
        } else if (argument == "--hot-delta-c") {
            if (!parse_float(value, &options->fusion.relative_hot_delta_c))
                return false;
        } else if (argument == "--hot-min-pixels") {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, &parsed))
                return false;
            options->fusion.minimum_hot_pixels =
                static_cast<std::size_t>(parsed);
        } else if (argument == "--mapping-margin-px") {
            float parsed = 0.0F;
            if (!parse_float(value, &parsed))
                return false;
            options->fusion.mapping_margin_px = parsed;
        } else if (argument == "--minimum-overlap") {
            float parsed = 0.0F;
            if (!parse_float(value, &parsed))
                return false;
            options->fusion.minimum_thermal_overlap = parsed;
        } else if (argument == "--confirm-frames") {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, &parsed) ||
                parsed > std::numeric_limits<std::uint32_t>::max())
                return false;
            options->debounce.confirmation_frames =
                static_cast<std::uint32_t>(parsed);
        } else if (argument == "--release-frames") {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, &parsed) ||
                parsed > std::numeric_limits<std::uint32_t>::max())
                return false;
            options->debounce.release_frames =
                static_cast<std::uint32_t>(parsed);
        } else if (argument == "--cooldown-ms") {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, &parsed) ||
                parsed > std::numeric_limits<std::uint64_t>::max() /
                             1'000'000ULL)
                return false;
            options->debounce.cooldown_ns = parsed * 1'000'000ULL;
        } else if (argument == "--lookahead-ms") {
            if (!parse_u64(value, &options->lookahead_ms))
                return false;
        } else if (argument == "--max-skew-ms") {
            if (!parse_u64(value, &options->maximum_skew_ms))
                return false;
        } else if (argument == "--overlay-ttl-ms") {
            if (!parse_u64(value, &options->overlay_ttl_ms))
                return false;
        } else if (argument == "--stream-bitrate-bps") {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, &parsed) || parsed == 0U ||
                parsed > std::numeric_limits<std::uint32_t>::max())
                return false;
            options->stream_bitrate_bps =
                static_cast<std::uint32_t>(parsed);
        } else {
            return false;
        }
    }
    options->fusion.minimum_person_confidence =
        options->confidence_threshold;
    return !options->eeprom.empty() && !options->model.empty() &&
        !options->calibration.empty() && !options->summary_json.empty() &&
        !options->event_csv.empty() && options->duration_seconds > 0 &&
        options->duration_seconds <= 86400 &&
        options->confidence_threshold > 0.0F &&
        options->confidence_threshold < 1.0F &&
        options->fusion.minimum_hot_pixels > 0 &&
        options->fusion.minimum_hot_pixels <= p2::kMlx90640Pixels &&
        options->fusion.relative_hot_delta_c >= 0.0F &&
        options->fusion.mapping_margin_px >= 0.0 &&
        options->fusion.minimum_thermal_overlap >= 0.0 &&
        options->fusion.minimum_thermal_overlap <= 1.0 &&
        options->debounce.confirmation_frames > 0 &&
        options->debounce.release_frames > 0 &&
        options->lookahead_ms <= 1000 && options->maximum_skew_ms <= 1000 &&
        options->overlay_ttl_ms > 0 && options->overlay_ttl_ms <= 10'000;
}

bool read_file(const std::string &path, std::vector<std::uint8_t> *bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes->assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

double percentile(std::vector<double> values, double quantile)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(std::ceil(
        quantile * static_cast<double>(values.size()))) - 1U;
    return values[std::min(index, values.size() - 1U)];
}

double maximum(const std::vector<double> &values)
{
    return values.empty() ? 0.0
                          : *std::max_element(values.begin(), values.end());
}

std::string json_escape(const std::string &value)
{
    std::string escaped;
    for (const char character : value) {
        if (character == '\\' || character == '"')
            escaped.push_back('\\');
        if (character == '\n') {
            escaped += "\\n";
            continue;
        }
        escaped.push_back(character);
    }
    return escaped;
}

void write_summary(
    std::ostream &output, const Options &options,
    const p2::CrossSpectralCalibration &calibration,
    const p2::PersonDetectorRuntimeInfo &runtime,
    const p2::VisibleCaptureStats &visible_capture,
    const p2::ThermalCaptureStats &thermal_capture,
    const p2::BoundedQueueStats &visible_queue,
    const p2::BoundedQueueStats &encoder_queue,
    const p2::BoundedQueueStats &fusion_queue,
    const p2::TemporalFusionJoinerStats &joiner,
    const p2::ThermalTemporalFilterStats &filter,
    const InferenceStats &inference,
    const ThermalProcessingStats &thermal,
    const FusionStats &fusion,
    const p2::MppH264EncoderStats &encoder,
    const p2::RtspPublisherStats &publisher,
    double elapsed_seconds, bool passed, const std::string &error)
{
    const bool streaming_enabled = !options.rtsp_url.empty() ||
        !options.h264_output.empty();
    const double encoded_frames =
        static_cast<double>(encoder.encoded_frames);
    output << std::fixed << std::setprecision(3)
           << "{\n"
           << "  \"schema\": \"p2.multimodal-fusion.v2\",\n"
           << "  \"result\": \"" << (passed ? "PASS" : "FAIL")
           << "\",\n"
           << "  \"error\": \"" << json_escape(error) << "\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT
           << "\",\n"
           << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n"
           << "  \"visible_device\": \""
           << json_escape(options.visible) << "\",\n"
           << "  \"thermal_device\": \""
           << json_escape(options.thermal) << "\",\n"
           << "  \"model_sha256_expected\": \"" << kModelSha256
           << "\",\n"
           << "  \"rknn_api_version\": \""
           << json_escape(runtime.rknn_api_version) << "\",\n"
           << "  \"rknn_driver_version\": \""
           << json_escape(runtime.rknn_driver_version) << "\",\n"
           << "  \"calibration\": {\"id\": \""
           << json_escape(calibration.calibration_id)
           << "\", \"nominal_distance_mm\": "
           << calibration.nominal_distance_mm
           << ", \"fit_max_error_px\": "
           << calibration.max_reprojection_error_px
           << ", \"provisional\": true},\n"
           << "  \"fusion_config\": {\"hot_absolute_c\": "
           << options.fusion.absolute_hot_temperature_c
           << ", \"hot_delta_c\": "
           << options.fusion.relative_hot_delta_c
           << ", \"hot_min_pixels\": "
           << options.fusion.minimum_hot_pixels
           << ", \"mapping_margin_px\": "
           << options.fusion.mapping_margin_px
           << ", \"minimum_overlap\": "
           << options.fusion.minimum_thermal_overlap
           << ", \"confirm_frames\": "
           << options.debounce.confirmation_frames
           << ", \"release_frames\": "
           << options.debounce.release_frames
           << ", \"cooldown_ms\": "
           << options.debounce.cooldown_ns / 1'000'000ULL << "},\n"
           << "  \"visible_capture\": {\"dqbuf\": "
           << visible_capture.dqbuf_count
           << ", \"sequence_gaps\": " << visible_capture.sequence_gaps
           << ", \"bad_bytes_used\": " << visible_capture.bad_bytes_used
           << ", \"missing_timestamp\": "
           << visible_capture.missing_monotonic_timestamp
           << ", \"exported_dma_buffers\": "
           << visible_capture.exported_dma_buffers << "},\n"
           << "  \"thermal_capture\": {\"valid_pairs\": "
           << thermal_capture.valid_pairs
           << ", \"sequence_gaps\": " << thermal_capture.sequence_gaps
           << ", \"invalid_payloads\": "
           << thermal_capture.invalid_payloads
           << ", \"timestamp_mismatches\": "
           << thermal_capture.timestamp_mismatches << "},\n"
           << "  \"visible_queue\": {\"pushed\": "
           << visible_queue.pushed << ", \"popped\": "
           << visible_queue.popped << ", \"drop_oldest\": "
           << visible_queue.dropped_oldest << ", \"pending\": "
           << visible_queue.pending << "},\n"
           << "  \"fusion_queue\": {\"pushed\": "
           << fusion_queue.pushed << ", \"popped\": "
           << fusion_queue.popped << ", \"drop_oldest\": "
           << fusion_queue.dropped_oldest << ", \"pending\": "
           << fusion_queue.pending << "},\n"
           << "  \"streaming\": {\"enabled\": "
           << (streaming_enabled ? "true" : "false")
           << ", \"rtsp_url\": \"" << json_escape(options.rtsp_url)
           << "\", \"output_width\": 1080, \"output_height\": 1920, "
              "\"bitrate_bps\": " << options.stream_bitrate_bps
           << ", \"encoder_queue_pushed\": " << encoder_queue.pushed
           << ", \"encoder_queue_popped\": " << encoder_queue.popped
           << ", \"encoder_queue_drop_oldest\": "
           << encoder_queue.dropped_oldest
           << ", \"encoded_frames\": " << encoder.encoded_frames
           << ", \"encoded_bytes\": " << encoder.encoded_bytes
           << ", \"key_frames\": " << encoder.key_frames
           << ", \"osd_frames\": " << encoder.osd_frames
           << ", \"source_dma_buf_imports\": "
           << encoder.source_dma_buf_imports
           << ", \"average_rga_ms\": "
           << (encoded_frames == 0.0 ? 0.0 :
               encoder.rga_total_ms / encoded_frames)
           << ", \"average_mpp_ms\": "
           << (encoded_frames == 0.0 ? 0.0 :
               encoder.mpp_total_ms / encoded_frames)
           << ", \"published_packets\": " << publisher.packets
           << ", \"publish_failures\": " << publisher.failures << "},\n"
           << "  \"temporal_join\": {\"visible_received\": "
           << joiner.synchronizer.visible_received
           << ", \"thermal_received\": "
           << joiner.synchronizer.thermal_received
           << ", \"matched\": " << joiner.synchronizer.matched
           << ", \"unmatched_skew\": "
           << joiner.synchronizer.unmatched_skew
           << ", \"emitted_pairs\": " << joiner.emitted_pairs
           << ", \"missing_visible_results\": "
           << joiner.missing_visible_results
           << ", \"missing_thermal_results\": "
           << joiner.missing_thermal_results << "},\n"
           << "  \"processing\": {\"inference_frames\": "
           << inference.frames << ", \"inference_failures\": "
           << inference.failures << ", \"person_detections\": "
           << inference.person_detections
           << ", \"thermal_frames\": " << thermal.frames
           << ", \"math_failures\": " << thermal.math_failures
           << ", \"filter_failures\": " << thermal.filter_failures
           << ", \"repaired_nonfinite_temperature_pixels\": "
           << thermal.repaired_nonfinite_temperature_pixels
           << ", \"repaired_nonfinite_image_pixels\": "
           << thermal.repaired_nonfinite_image_pixels
           << ", \"filter_accepted_frames\": "
           << filter.accepted_frames << "},\n"
           << "  \"fusion\": {\"processed_pairs\": " << fusion.pairs
           << ", \"failures\": " << fusion.failures
           << ", \"evidence_frames\": " << fusion.evidence_frames
           << ", \"active_frames\": " << fusion.active_frames
           << ", \"activations\": " << fusion.activations
           << ", \"deactivations\": " << fusion.deactivations
           << ", \"people\": " << fusion.total_people
           << ", \"hot_regions\": " << fusion.total_hot_regions
           << ", \"matches\": " << fusion.total_matches << "},\n"
           << "  \"latency\": {\"absolute_skew_p50_ms\": "
           << percentile(fusion.absolute_skew_ms, 0.50)
           << ", \"absolute_skew_p95_ms\": "
           << percentile(fusion.absolute_skew_ms, 0.95)
           << ", \"absolute_skew_max_ms\": "
           << maximum(fusion.absolute_skew_ms)
           << ", \"inference_total_p50_ms\": "
           << percentile(inference.total_ms, 0.50)
           << ", \"inference_total_p95_ms\": "
           << percentile(inference.total_ms, 0.95)
           << ", \"fusion_p50_ms\": "
           << percentile(fusion.fusion_ms, 0.50)
           << ", \"fusion_p95_ms\": "
           << percentile(fusion.fusion_ms, 0.95) << "}\n"
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
    if ((options.visible.empty() || options.thermal.empty()) &&
        !p2::discover_devices(&devices, &error)) {
        std::cerr << "device discovery failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    if (options.visible.empty())
        options.visible = devices.visible;
    if (options.thermal.empty())
        options.thermal = devices.thermal;

    p2::CrossSpectralCalibration calibration;
    if (!p2::load_cross_spectral_calibration(
            options.calibration, &calibration, &error) ||
        !p2::validate_cross_spectral_calibration(calibration, &error)) {
        std::cerr << "calibration load failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    std::vector<std::uint8_t> eeprom;
    if (!read_file(options.eeprom, &eeprom)) {
        std::cerr << "failed to read EEPROM " << options.eeprom << '\n';
        return EXIT_FAILURE;
    }
    p2::Mlx90640Math math;
    if (!math.initialize_eeprom_be(eeprom.data(), eeprom.size(), &error)) {
        std::cerr << "thermal math initialization failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    p2::PersonDetectorConfig detector_config;
    detector_config.model_path = options.model;
    detector_config.rotation = options.rotation;
    detector_config.confidence_threshold = options.confidence_threshold;
    detector_config.nms_threshold = options.nms_threshold;
    detector_config.use_io_mem = true;
    p2::RknnPersonDetector detector(detector_config);
    if (!detector.initialize(&error)) {
        std::cerr << "person detector initialization failed: " << error
                  << '\n';
        return EXIT_FAILURE;
    }
    const p2::PersonDetectorRuntimeInfo runtime = detector.runtime_info();

    const bool streaming_enabled = !options.rtsp_url.empty() ||
        !options.h264_output.empty();
    std::unique_ptr<p2::MppH264Encoder> encoder;
    std::unique_ptr<p2::RtspPublisher> publisher;
    std::ofstream h264_output;
    if (streaming_enabled) {
        p2::MppH264EncoderConfig encoder_config;
        encoder_config.bitrate_bps = options.stream_bitrate_bps;
        encoder_config.rotation = options.rotation;
        encoder = std::make_unique<p2::MppH264Encoder>(encoder_config);
        if (!encoder->initialize(&error)) {
            std::cerr << "stream encoder initialization failed: "
                      << error << '\n';
            return EXIT_FAILURE;
        }
        if (!options.rtsp_url.empty()) {
            p2::RtspPublisherConfig publisher_config;
            publisher_config.url = options.rtsp_url;
            publisher = std::make_unique<p2::RtspPublisher>(
                publisher_config);
            if (!publisher->connect(encoder->codec_header(), &error)) {
                std::cerr << "RTSP publisher connection failed: "
                          << error << '\n';
                return EXIT_FAILURE;
            }
        }
        if (!options.h264_output.empty()) {
            h264_output.open(options.h264_output, std::ios::binary);
            if (!h264_output) {
                std::cerr << "failed to open H.264 output "
                          << options.h264_output << '\n';
                return EXIT_FAILURE;
            }
            const std::vector<std::uint8_t> &header =
                encoder->codec_header();
            h264_output.write(
                reinterpret_cast<const char *>(header.data()),
                static_cast<std::streamsize>(header.size()));
        }
    }

    std::ofstream events(options.event_csv);
    if (!events) {
        std::cerr << "failed to open event CSV " << options.event_csv << '\n';
        return EXIT_FAILURE;
    }
    events << "timestamp_ns,visible_sequence,thermal_sequence,signed_skew_ns,"
              "person_count,hot_region_count,match_count,background_c,"
              "activation_c,maximum_matched_c,evidence,alert_state,"
              "transition,active,fusion_ms\n";

    p2::ThermalMathConfig math_config;
    math_config.emissivity = options.emissivity;
    math_config.reflected_temperature_offset_c = options.reflected_offset_c;
    p2::ThermalTemporalFilterConfig filter_config;
    filter_config.time_constant_ms = options.filter_tau_ms;
    filter_config.reset_gap_ns =
        options.filter_reset_gap_ms * 1'000'000ULL;
    p2::ThermalTemporalFilter filter(filter_config);
    p2::TemporalFusionJoinerConfig joiner_config;
    joiner_config.synchronizer.lookahead_ns =
        options.lookahead_ms * 1'000'000ULL;
    joiner_config.synchronizer.max_skew_ns =
        options.maximum_skew_ms * 1'000'000ULL;
    p2::TemporalFusionJoiner joiner(joiner_config);
    p2::AlertDebouncer debouncer(options.debounce);
    using SharedVisibleLease = std::shared_ptr<p2::VisibleFrameLease>;
    p2::BoundedLatestQueue<SharedVisibleLease> visible_queue(1);
    p2::BoundedLatestQueue<SharedVisibleLease> encoder_queue(1);
    p2::BoundedLatestQueue<p2::FusionInputPair> fusion_queue(8);
    p2::VisibleCapture visible_capture({
        options.visible, 3840, 2160, kCaptureBufferCount, 500, true,
    });
    p2::ThermalCapture thermal_capture({options.thermal, 4, 1000});
    InferenceStats inference_stats;
    ThermalProcessingStats thermal_stats;
    FusionStats fusion_stats;
    OverlayState latest_overlay;
    latest_overlay.overlay.banner = "P2 MONITOR";
    latest_overlay.overlay.banner_color = p2::OverlayColor::green;
    std::mutex overlay_mutex;
    std::mutex failure_mutex;
    std::string failure;
    const std::uint64_t started_ns = p2::monotonic_now_ns();
    const std::uint64_t duration_ns =
        options.duration_seconds * 1'000'000'000ULL;
    const std::uint64_t deadline_ns = started_ns + duration_ns;

    const auto set_failure = [&](const std::string &message) {
        {
            std::lock_guard<std::mutex> lock(failure_mutex);
            if (failure.empty())
                failure = message;
        }
        g_stop.store(true);
    };
    const auto enqueue_pairs = [&](std::vector<p2::FusionInputPair> pairs) {
        for (p2::FusionInputPair &pair : pairs) {
            if (!fusion_queue.push(std::move(pair))) {
                set_failure("fusion queue closed while producer was active");
                return;
            }
        }
    };

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    bool visible_capture_ok = false;
    bool thermal_capture_ok = false;
    std::thread visible_thread([&]() {
        std::string capture_error;
        visible_capture_ok = visible_capture.run_leased_frames(
            g_stop,
            [&](p2::VisibleFrameLease &&lease) {
                if (p2::monotonic_now_ns() >= deadline_ns) {
                    g_stop.store(true);
                    return;
                }
                SharedVisibleLease shared =
                    std::make_shared<p2::VisibleFrameLease>(
                        std::move(lease));
                if (!visible_queue.push(shared)) {
                    set_failure("visible queue closed during capture");
                    return;
                }
                if (streaming_enabled &&
                    !encoder_queue.push(std::move(shared)))
                    set_failure("encoder queue closed during capture");
            },
            &capture_error);
        if (!visible_capture_ok)
            set_failure("visible capture failed: " + capture_error);
        visible_queue.close(false);
        encoder_queue.close(false);
    });

    std::thread inference_thread([&]() {
        SharedVisibleLease lease;
        while (visible_queue.wait_pop(&lease)) {
            const p2::VisibleFrameView &frame = lease->view();
            p2::PersonInferenceResult result;
            std::string inference_error;
            if (frame.data_offset != 0U || frame.dma_buf_fd < 0 ||
                !detector.infer_nv12_dmabuf(
                    frame.dma_buf_fd, frame.size, frame.width, frame.height,
                    frame.bytes_per_line, &result, &inference_error)) {
                ++inference_stats.failures;
                set_failure("person inference failed: " + inference_error);
                lease.reset();
                break;
            }
            ++inference_stats.frames;
            inference_stats.person_detections += result.detections.size();
            inference_stats.total_ms.push_back(result.timing.total_ms);
            p2::VisibleFusionFrame fusion_frame;
            fusion_frame.event = frame.event;
            fusion_frame.timing = result.timing;
            fusion_frame.detections = std::move(result.detections);
            fusion_frame.inference_done_ns = p2::monotonic_now_ns();
            lease.reset();
            enqueue_pairs(joiner.push_visible(std::move(fusion_frame)));
        }
        lease.reset();
    });

    std::thread encoder_thread;
    if (streaming_enabled) {
        encoder_thread = std::thread([&]() {
            SharedVisibleLease lease;
            while (encoder_queue.wait_pop(&lease)) {
                const p2::VisibleFrameView &frame = lease->view();
                OverlayState snapshot;
                {
                    std::lock_guard<std::mutex> lock(overlay_mutex);
                    snapshot = latest_overlay;
                }
                const std::uint64_t ttl_ns =
                    options.overlay_ttl_ms * 1'000'000ULL;
                if (snapshot.visible_timestamp_ns == 0U ||
                    frame.event.timestamp_ns <
                        snapshot.visible_timestamp_ns ||
                    frame.event.timestamp_ns -
                        snapshot.visible_timestamp_ns > ttl_ns) {
                    snapshot.overlay = {};
                    snapshot.overlay.banner = "P2 MONITOR";
                    snapshot.overlay.banner_color =
                        p2::OverlayColor::green;
                }
                p2::EncodedH264Packet packet;
                std::string stream_error;
                if (frame.data_offset != 0U || frame.dma_buf_fd < 0 ||
                    !encoder->encode_nv12_dmabuf(
                        frame.dma_buf_fd, frame.size,
                        frame.width, frame.height,
                        frame.bytes_per_line, frame.event.timestamp_ns,
                        snapshot.overlay, &packet, &stream_error)) {
                    set_failure("MPP stream encode failed: " +
                                stream_error);
                    lease.reset();
                    break;
                }
                lease.reset();
                if (h264_output.is_open()) {
                    h264_output.write(
                        reinterpret_cast<const char *>(
                            packet.bytes.data()),
                        static_cast<std::streamsize>(
                            packet.bytes.size()));
                    if (!h264_output) {
                        set_failure("H.264 evidence write failed");
                        break;
                    }
                }
                if (publisher != nullptr &&
                    !publisher->publish(packet, &stream_error)) {
                    set_failure("RTSP publish failed: " + stream_error);
                    break;
                }
            }
            lease.reset();
        });
    }

    std::thread thermal_thread([&]() {
        std::string capture_error;
        thermal_capture_ok = thermal_capture.run(
            g_stop,
            [&](const p2::ThermalFramePayload &frame) {
                if (p2::monotonic_now_ns() >= deadline_ns) {
                    g_stop.store(true);
                    return;
                }
                p2::ThermalMathResult result;
                std::string processing_error;
                if (!math.calculate(frame, math_config, &result,
                                    &processing_error)) {
                    ++thermal_stats.math_failures;
                    set_failure("thermal math failed: " + processing_error);
                    return;
                }
                std::array<float, p2::kMlx90640Pixels> filtered;
                if (!filter.process(frame.event.timestamp_ns,
                                    result.temperature_c, &filtered,
                                    &processing_error)) {
                    ++thermal_stats.filter_failures;
                    set_failure("thermal filter failed: " + processing_error);
                    return;
                }
                ++thermal_stats.frames;
                thermal_stats.repaired_nonfinite_temperature_pixels +=
                    result.repaired_nonfinite_temperature_pixels;
                thermal_stats.repaired_nonfinite_image_pixels +=
                    result.repaired_nonfinite_image_pixels;
                p2::ThermalFusionFrame fusion_frame;
                fusion_frame.event = frame.event;
                fusion_frame.temperature_c = filtered;
                fusion_frame.ambient_temperature_c = result.ta_c;
                fusion_frame.processing_done_ns = p2::monotonic_now_ns();
                enqueue_pairs(joiner.push_thermal(std::move(fusion_frame)));
            },
            &capture_error);
        if (!thermal_capture_ok)
            set_failure("thermal capture failed: " + capture_error);
    });

    std::thread supervisor([&]() {
        visible_thread.join();
        inference_thread.join();
        if (encoder_thread.joinable())
            encoder_thread.join();
        thermal_thread.join();
        enqueue_pairs(joiner.flush());
        fusion_queue.close(false);
    });

    p2::FusionInputPair pair;
    while (fusion_queue.wait_pop(&pair)) {
        const auto fusion_started = std::chrono::steady_clock::now();
        p2::MultimodalFusionResult result;
        std::string fusion_error;
        if (!p2::analyze_multimodal_frame(
                calibration, pair.thermal.temperature_c,
                pair.visible.detections, options.fusion, &result,
                &fusion_error)) {
            ++fusion_stats.failures;
            set_failure("multimodal fusion failed: " + fusion_error);
            continue;
        }
        p2::AlertUpdate alert;
        if (!debouncer.update(pair.thermal.event.timestamp_ns,
                              result.person_with_thermal_evidence,
                              &alert, &fusion_error)) {
            ++fusion_stats.failures;
            set_failure("alert debounce failed: " + fusion_error);
            continue;
        }
        if (streaming_enabled) {
            OverlayState next_overlay;
            next_overlay.visible_timestamp_ns =
                pair.visible.event.timestamp_ns;
            std::vector<float> matched_temperatures(
                pair.visible.detections.size(),
                std::numeric_limits<float>::quiet_NaN());
            for (const p2::PersonThermalMatch &match : result.matches) {
                if (match.person_index < matched_temperatures.size() &&
                    (!std::isfinite(
                         matched_temperatures[match.person_index]) ||
                     match.max_temperature_c >
                         matched_temperatures[match.person_index]))
                    matched_temperatures[match.person_index] =
                        match.max_temperature_c;
            }
            if (!p2::make_person_video_overlay(
                    pair.visible.detections, matched_temperatures,
                    alert.active, 3840, 2160, 1080, 1920,
                    options.rotation, &next_overlay.overlay,
                    &fusion_error)) {
                ++fusion_stats.failures;
                set_failure("video overlay construction failed: " +
                            fusion_error);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(overlay_mutex);
                latest_overlay = std::move(next_overlay);
            }
        }
        const double fusion_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - fusion_started).count();
        ++fusion_stats.pairs;
        fusion_stats.total_people += pair.visible.detections.size();
        fusion_stats.total_hot_regions += result.hot_regions.size();
        fusion_stats.total_matches += result.matches.size();
        if (result.person_with_thermal_evidence)
            ++fusion_stats.evidence_frames;
        if (alert.active)
            ++fusion_stats.active_frames;
        if (alert.transition == p2::AlertTransition::activated)
            ++fusion_stats.activations;
        if (alert.transition == p2::AlertTransition::deactivated)
            ++fusion_stats.deactivations;
        fusion_stats.absolute_skew_ms.push_back(
            static_cast<double>(pair.match.absolute_delta_ns) / 1.0e6);
        fusion_stats.fusion_ms.push_back(fusion_ms);
        float maximum_matched = 0.0F;
        for (const p2::PersonThermalMatch &match : result.matches)
            maximum_matched = std::max(maximum_matched,
                                       match.max_temperature_c);
        events << pair.thermal.event.timestamp_ns << ','
               << pair.visible.event.sequence << ','
               << pair.thermal.event.sequence << ','
               << pair.match.signed_delta_ns << ','
               << pair.visible.detections.size() << ','
               << result.hot_regions.size() << ','
               << result.matches.size() << ','
               << result.background_temperature_c << ','
               << result.activation_temperature_c << ','
               << maximum_matched << ','
               << (result.person_with_thermal_evidence ? 1 : 0) << ','
               << p2::to_string(alert.state) << ','
               << p2::to_string(alert.transition) << ','
               << (alert.active ? 1 : 0) << ',' << fusion_ms << '\n';
        if (options.maximum_pairs != 0 &&
            fusion_stats.pairs >= options.maximum_pairs)
            g_stop.store(true);
    }
    supervisor.join();
    if (publisher != nullptr)
        publisher->close();
    if (h264_output.is_open()) {
        h264_output.flush();
        if (!h264_output)
            set_failure("H.264 evidence flush failed");
    }
    const double elapsed_seconds = static_cast<double>(
        p2::monotonic_now_ns() - started_ns) / 1.0e9;
    {
        std::lock_guard<std::mutex> lock(failure_mutex);
        error = failure;
    }
    const p2::VisibleCaptureStats visible_capture_stats =
        visible_capture.stats();
    const p2::ThermalCaptureStats thermal_capture_stats =
        thermal_capture.stats();
    const p2::TemporalFusionJoinerStats joiner_stats = joiner.stats();
    const p2::MppH264EncoderStats encoder_stats =
        encoder != nullptr ? encoder->stats()
                           : p2::MppH264EncoderStats{};
    const p2::RtspPublisherStats publisher_stats =
        publisher != nullptr ? publisher->stats()
                             : p2::RtspPublisherStats{};
    const bool passed = error.empty() && visible_capture_ok &&
        thermal_capture_ok && fusion_stats.pairs > 0 &&
        inference_stats.failures == 0 && thermal_stats.math_failures == 0 &&
        thermal_stats.filter_failures == 0 && fusion_stats.failures == 0 &&
        visible_capture_stats.bad_bytes_used == 0 &&
        visible_capture_stats.missing_monotonic_timestamp == 0 &&
        thermal_capture_stats.invalid_payloads == 0 &&
        thermal_capture_stats.timestamp_mismatches == 0 &&
        joiner_stats.missing_visible_results == 0 &&
        joiner_stats.missing_thermal_results == 0 &&
        (!streaming_enabled ||
         (encoder_stats.encoded_frames > 0U &&
          encoder_stats.osd_frames == encoder_stats.encoded_frames &&
          (publisher == nullptr ||
           (publisher_stats.packets == encoder_stats.encoded_frames &&
            publisher_stats.failures == 0U))));

    write_summary(std::cout, options, calibration, runtime,
                  visible_capture_stats, thermal_capture_stats,
                  visible_queue.stats(), encoder_queue.stats(),
                  fusion_queue.stats(), joiner_stats,
                  filter.stats(), inference_stats, thermal_stats,
                  fusion_stats, encoder_stats, publisher_stats,
                  elapsed_seconds, passed, error);
    std::ofstream summary(options.summary_json);
    if (!summary) {
        std::cerr << "failed to open summary JSON " << options.summary_json
                  << '\n';
        return EXIT_FAILURE;
    }
    write_summary(summary, options, calibration, runtime,
                  visible_capture_stats, thermal_capture_stats,
                  visible_queue.stats(), encoder_queue.stats(),
                  fusion_queue.stats(), joiner_stats,
                  filter.stats(), inference_stats, thermal_stats,
                  fusion_stats, encoder_stats, publisher_stats,
                  elapsed_seconds, passed, error);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
