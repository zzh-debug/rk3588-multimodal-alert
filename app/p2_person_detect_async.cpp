#include "p2/capture/device_discovery.hpp"
#include "p2/capture/v4l2_utils.hpp"
#include "p2/capture/visible_capture.hpp"
#include "p2/inference/person_detector.hpp"
#include "p2/pipeline/bounded_queue.hpp"

#include <algorithm>
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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

constexpr const char *kModelSha256 =
    "7c6b801de602b8aaa72269fab8daab21810418a4b01c8e8995313916e278fe71";
constexpr std::uint32_t kCaptureBufferCount = 6;

std::atomic<bool> g_stop{false};

struct Options {
    std::string visible;
    std::string model = "/usr/share/p2/models/yolov5s-640-640.rknn";
    std::string json_output;
    std::string detection_csv;
    std::string source_memory = "dmabuf";
    std::string rknn_input = "io-mem";
    std::uint64_t duration_seconds = 60;
    std::uint64_t maximum_frames = 0;
    std::uint64_t warmup_frames = 10;
    std::uint64_t source_equivalence_frames = 0;
    std::size_t queue_capacity = 1;
    p2::ImageRotation rotation = p2::ImageRotation::kClockwise270;
    float confidence_threshold = 0.25F;
    float nms_threshold = 0.45F;
};

struct RunStats {
    std::uint64_t inference_frames = 0;
    std::uint64_t measured_frames = 0;
    std::uint64_t frames_with_person = 0;
    std::uint64_t person_detections = 0;
    std::uint64_t processed_sequence_gap_events = 0;
    std::uint64_t processed_sequence_gap_frames = 0;
    std::uint64_t source_equivalence_checks = 0;
    std::uint64_t source_equivalence_mismatches = 0;
    bool have_previous_sequence = false;
    std::uint32_t previous_sequence = 0;
    std::vector<double> queue_age_ms;
    std::vector<double> end_to_end_ms;
    std::vector<double> rga_import_ms;
    std::vector<double> rga_ms;
    std::vector<double> rknn_ms;
    std::vector<double> post_ms;
    std::vector<double> total_ms;
};

void signal_handler(int)
{
    g_stop.store(true);
}

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --visible PATH       override discovered RKISP mainpath\n"
        << "  --model FILE         RK3588 YOLOv5s RKNN model\n"
        << "  --duration-sec N     run time, default 60\n"
        << "  --frames N           measured frame limit, 0 means duration only\n"
        << "  --warmup N           warm-up frames excluded from timing, default 10\n"
        << "  --queue-capacity N   drop-oldest queue size, 1..4, default 1\n"
        << "  --source-memory M    mmap or dmabuf; default dmabuf\n"
        << "  --rknn-input M       inputs-set or io-mem; default io-mem\n"
        << "  --verify-source N   compare dmabuf/mmap results for N frames\n"
        << "  --rotation VALUE     0, 90cw, 180 or 270cw; default 270cw\n"
        << "  --confidence VALUE   person confidence threshold, default 0.25\n"
        << "  --nms VALUE          person NMS IoU threshold, default 0.45\n"
        << "  --json FILE          write summary JSON\n"
        << "  --detections FILE    write per-person CSV\n"
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
        else if (argument == "--model")
            options->model = value;
        else if (argument == "--json")
            options->json_output = value;
        else if (argument == "--detections")
            options->detection_csv = value;
        else if (argument == "--source-memory")
            options->source_memory = value;
        else if (argument == "--rknn-input")
            options->rknn_input = value;
        else if (argument == "--duration-sec") {
            if (!parse_u64(value, &options->duration_seconds) ||
                options->duration_seconds == 0)
                return false;
        } else if (argument == "--frames") {
            if (!parse_u64(value, &options->maximum_frames))
                return false;
        } else if (argument == "--warmup") {
            if (!parse_u64(value, &options->warmup_frames))
                return false;
        } else if (argument == "--queue-capacity") {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, &parsed))
                return false;
            options->queue_capacity = static_cast<std::size_t>(parsed);
        } else if (argument == "--verify-source") {
            if (!parse_u64(value, &options->source_equivalence_frames))
                return false;
        } else if (argument == "--rotation") {
            if (!p2::parse_image_rotation(value, &options->rotation))
                return false;
        } else if (argument == "--confidence") {
            if (!parse_float(value, &options->confidence_threshold))
                return false;
        } else if (argument == "--nms") {
            if (!parse_float(value, &options->nms_threshold))
                return false;
        } else {
            return false;
        }
    }
    return !options->model.empty() &&
        (options->source_memory == "mmap" ||
         options->source_memory == "dmabuf") &&
        (options->rknn_input == "inputs-set" ||
         options->rknn_input == "io-mem") &&
        (options->source_memory == "dmabuf" ||
         options->rknn_input == "inputs-set") &&
        (options->source_memory == "dmabuf" ||
         options->source_equivalence_frames == 0U) &&
        options->duration_seconds <= 86400U &&
        options->warmup_frames <= 1000U && options->queue_capacity >= 1U &&
        options->source_equivalence_frames <= 100U &&
        options->queue_capacity <= 4U &&
        options->confidence_threshold > 0.0F &&
        options->confidence_threshold < 1.0F && options->nms_threshold > 0.0F &&
        options->nms_threshold < 1.0F;
}

bool inference_results_equivalent(const p2::PersonInferenceResult &left,
                                  const p2::PersonInferenceResult &right)
{
    if (left.detections.size() != right.detections.size())
        return false;
    constexpr float tolerance = 1.0e-4F;
    for (std::size_t index = 0; index < left.detections.size(); ++index) {
        const p2::PersonDetection &a = left.detections[index];
        const p2::PersonDetection &b = right.detections[index];
        if (std::fabs(a.confidence - b.confidence) > tolerance ||
            std::fabs(a.box.left - b.box.left) > tolerance ||
            std::fabs(a.box.top - b.box.top) > tolerance ||
            std::fabs(a.box.right - b.box.right) > tolerance ||
            std::fabs(a.box.bottom - b.box.bottom) > tolerance)
            return false;
    }
    return true;
}

double percentile(std::vector<double> values, double quantile)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double position = quantile * static_cast<double>(values.size() - 1U);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

std::string json_escape(const std::string &text)
{
    std::string escaped;
    for (const char character : text) {
        if (character == '\\' || character == '"')
            escaped.push_back('\\');
        escaped.push_back(character);
    }
    return escaped;
}

void write_latency_json(std::ostream &output,
                        const char *name,
                        const std::vector<double> &values,
                        bool trailing_comma)
{
    output << "    \"" << name << "\": {\"p50_ms\": "
           << percentile(values, 0.50) << ", \"p95_ms\": "
           << percentile(values, 0.95) << ", \"p99_ms\": "
           << percentile(values, 0.99) << ", \"max_ms\": "
           << (values.empty() ? 0.0
                              : *std::max_element(values.begin(), values.end()))
           << "}" << (trailing_comma ? "," : "") << "\n";
}

void write_summary(std::ostream &output,
                   const Options &options,
                   const p2::PersonDetectorRuntimeInfo &runtime,
                   const p2::VisibleCaptureStats &capture,
                   const p2::BoundedQueueStats &queue,
                   const RunStats &stats,
                   double measured_elapsed_seconds,
                   double run_elapsed_seconds)
{
    output << std::fixed << std::setprecision(3);
    output << "{\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT << "\",\n"
           << "  \"visible_device\": \"" << json_escape(options.visible)
           << "\",\n"
           << "  \"model_path\": \"" << json_escape(options.model) << "\",\n"
           << "  \"model_sha256_expected\": \"" << kModelSha256 << "\",\n"
           << "  \"rotation\": \""
           << p2::image_rotation_name(options.rotation) << "\",\n"
           << "  \"rknn_api_version\": \""
           << json_escape(runtime.rknn_api_version) << "\",\n"
           << "  \"rknn_driver_version\": \""
           << json_escape(runtime.rknn_driver_version) << "\",\n"
           << "  \"transport\": {\"capture_memory\": \"V4L2_MMAP\", "
              "\"handoff\": \"buffer_lease\", \"rga_source\": \""
           << (options.source_memory == "dmabuf"
                   ? "V4L2_EXPBUF_importbuffer_fd"
                   : "MMAP_virtual_address")
           << "\", \"application_full_frame_cpu_copies\": 0, "
              "\"dmabuf_source\": "
           << (options.source_memory == "dmabuf" ? "true" : "false")
           << ", \"rknn_input\": \"" << options.rknn_input
           << "\", \"end_to_end_zero_copy\": false},\n"
           << "  \"capture\": {\"width\": " << capture.width
           << ", \"height\": " << capture.height
           << ", \"bytes_per_line\": " << capture.bytes_per_line
           << ", \"allocated_buffers\": " << capture.allocated_buffers
           << ", \"lease_high_watermark\": "
           << capture.lease_high_watermark
           << ", \"exported_dma_buffers\": "
           << capture.exported_dma_buffers
           << ", \"dqbuf_count\": " << capture.dqbuf_count
           << ", \"sequence_gaps\": " << capture.sequence_gaps
           << ", \"bad_bytes_used\": " << capture.bad_bytes_used
           << ", \"missing_monotonic_timestamp\": "
           << capture.missing_monotonic_timestamp << "},\n"
           << "  \"queue\": {\"capacity\": " << options.queue_capacity
           << ", \"pushed\": " << queue.pushed
           << ", \"popped\": " << queue.popped
           << ", \"controlled_drop_oldest\": " << queue.dropped_oldest
           << ", \"shutdown_drops\": " << queue.shutdown_drops
           << ", \"closed_rejections\": " << queue.closed_rejections
           << ", \"high_watermark\": " << queue.high_watermark
           << ", \"pending\": " << queue.pending << "},\n"
           << "  \"processed_sequence_gaps\": {\"events\": "
           << stats.processed_sequence_gap_events << ", \"frames\": "
           << stats.processed_sequence_gap_frames << "},\n"
           << "  \"source_equivalence\": {\"requested\": "
           << options.source_equivalence_frames << ", \"checks\": "
           << stats.source_equivalence_checks << ", \"mismatches\": "
           << stats.source_equivalence_mismatches << "},\n"
           << "  \"run_elapsed_seconds\": " << run_elapsed_seconds << ",\n"
           << "  \"capture_fps\": "
           << (run_elapsed_seconds > 0.0
                   ? static_cast<double>(capture.dqbuf_count) /
                         run_elapsed_seconds
                   : 0.0)
           << ",\n"
           << "  \"inference_frames\": " << stats.inference_frames << ",\n"
           << "  \"warmup_frames\": " << options.warmup_frames << ",\n"
           << "  \"measured_frames\": " << stats.measured_frames << ",\n"
           << "  \"measured_elapsed_seconds\": "
           << measured_elapsed_seconds << ",\n"
           << "  \"measured_fps\": "
           << (measured_elapsed_seconds > 0.0
                   ? static_cast<double>(stats.measured_frames) /
                         measured_elapsed_seconds
                   : 0.0)
           << ",\n"
           << "  \"frames_with_person\": " << stats.frames_with_person
           << ",\n"
           << "  \"person_detections\": " << stats.person_detections
           << ",\n"
           << "  \"rga_dmabuf_import_count\": "
           << runtime.dma_buf_import_count << ",\n"
           << "  \"rknn_io_mem\": {\"enabled\": "
           << (runtime.io_mem_enabled ? "true" : "false")
           << ", \"width_stride\": " << runtime.input_width_stride
           << ", \"size_with_stride\": "
           << runtime.input_size_with_stride << ", \"non_cacheable\": "
           << (runtime.input_non_cacheable ? "true" : "false")
           << "},\n"
           << "  \"latency\": {\n";
    write_latency_json(output, "queue_age", stats.queue_age_ms, true);
    write_latency_json(output, "rga_dmabuf_import", stats.rga_import_ms,
                       true);
    write_latency_json(output, "rga", stats.rga_ms, true);
    write_latency_json(output, "rknn", stats.rknn_ms, true);
    write_latency_json(output, "postprocess", stats.post_ms, true);
    write_latency_json(output, "inference_total", stats.total_ms, true);
    write_latency_json(output, "capture_to_inference_end", stats.end_to_end_ms,
                       false);
    output << "  }\n}\n";
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::string media_device;
    std::string error;
    if (options.visible.empty() &&
        !p2::discover_visible_device(&options.visible, &media_device, &error)) {
        std::cerr << "visible discovery failed: " << error << '\n';
        return EXIT_FAILURE;
    }

    p2::PersonDetectorConfig detector_config;
    detector_config.model_path = options.model;
    detector_config.rotation = options.rotation;
    detector_config.confidence_threshold = options.confidence_threshold;
    detector_config.nms_threshold = options.nms_threshold;
    detector_config.use_io_mem = options.rknn_input == "io-mem";
    p2::RknnPersonDetector detector(detector_config);
    if (!detector.initialize(&error)) {
        std::cerr << "detector initialization failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    const p2::PersonDetectorRuntimeInfo &runtime = detector.runtime_info();
    std::unique_ptr<p2::RknnPersonDetector> verification_detector;
    if (options.source_equivalence_frames != 0U) {
        p2::PersonDetectorConfig verification_config = detector_config;
        verification_config.use_io_mem = false;
        verification_detector.reset(
            new p2::RknnPersonDetector(verification_config));
        if (!verification_detector->initialize(&error)) {
            std::cerr << "verification detector initialization failed: "
                      << error << '\n';
            return EXIT_FAILURE;
        }
    }
    std::cout << "P2.4 async person detector: visible=" << options.visible
              << " queue_capacity=" << options.queue_capacity
              << " rotation=" << p2::image_rotation_name(options.rotation)
              << " source_memory=" << options.source_memory
              << " rknn_input=" << options.rknn_input
              << " transport=V4L2_MMAP_buffer_lease"
              << " full_frame_cpu_copies=0\n";

    std::ofstream detection_csv;
    if (!options.detection_csv.empty()) {
        detection_csv.open(options.detection_csv);
        if (!detection_csv) {
            std::cerr << "cannot open detection CSV: "
                      << options.detection_csv << '\n';
            return EXIT_FAILURE;
        }
        detection_csv
            << "sequence,timestamp_ns,confidence,left,top,right,bottom,"
               "queue_age_ms,rga_import_ms,rga_ms,rknn_ms,postprocess_ms,"
               "total_ms,"
               "capture_to_inference_end_ms\n";
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    p2::BoundedLatestQueue<p2::VisibleFrameLease> queue(
        options.queue_capacity);
    p2::VisibleCapture capture({options.visible, 3840, 2160,
                                kCaptureBufferCount, 500,
                                options.source_memory == "dmabuf"});
    bool capture_ok = false;
    std::string capture_error;
    const auto run_begin = std::chrono::steady_clock::now();
    std::thread capture_thread([&]() {
        capture_ok = capture.run_leased_frames(
            g_stop,
            [&](p2::VisibleFrameLease &&lease) {
                if (!queue.push(std::move(lease)))
                    g_stop.store(true);
            },
            &capture_error);
        queue.close(false);
    });

    RunStats stats;
    std::string inference_error;
    auto measured_begin = run_begin;
    auto measured_end = run_begin;
    p2::VisibleFrameLease lease;
    while (queue.wait_pop(&lease)) {
        const p2::VisibleFrameView &frame = lease.view();
        if (stats.have_previous_sequence &&
            frame.event.sequence != stats.previous_sequence + 1U) {
            ++stats.processed_sequence_gap_events;
            stats.processed_sequence_gap_frames +=
                static_cast<std::uint32_t>(frame.event.sequence -
                                           stats.previous_sequence - 1U);
        }
        stats.previous_sequence = frame.event.sequence;
        stats.have_previous_sequence = true;

        const std::uint64_t inference_begin_ns = p2::monotonic_now_ns();
        const double queue_age_ms = inference_begin_ns >= frame.event.arrival_ns
            ? static_cast<double>(inference_begin_ns - frame.event.arrival_ns) /
                  1.0e6
            : 0.0;
        p2::PersonInferenceResult result;
        bool inference_ok = false;
        if (options.source_memory == "dmabuf") {
            if (frame.data_offset != 0U) {
                inference_error =
                    "RGA DMA-BUF source does not support nonzero data_offset";
            } else {
                inference_ok = detector.infer_nv12_dmabuf(
                    frame.dma_buf_fd, frame.size, frame.width, frame.height,
                    frame.bytes_per_line, &result, &inference_error);
                if (inference_ok && stats.source_equivalence_checks <
                                        options.source_equivalence_frames) {
                    p2::PersonInferenceResult mmap_result;
                    std::string mmap_error;
                    if (!verification_detector->infer_nv12(
                            frame.data, frame.size, frame.width, frame.height,
                            frame.bytes_per_line, &mmap_result, &mmap_error)) {
                        inference_error =
                            "MMAP equivalence inference failed: " + mmap_error;
                        inference_ok = false;
                    } else {
                        ++stats.source_equivalence_checks;
                        if (!inference_results_equivalent(result, mmap_result)) {
                            ++stats.source_equivalence_mismatches;
                            inference_error =
                                "DMA-BUF/MMAP inference result mismatch";
                            inference_ok = false;
                        }
                    }
                }
            }
        } else {
            inference_ok = detector.infer_nv12(
                frame.data, frame.size, frame.width, frame.height,
                frame.bytes_per_line, &result, &inference_error);
        }
        if (!inference_ok) {
            g_stop.store(true);
            lease.reset();
            queue.close(true);
            break;
        }
        const std::uint64_t inference_end_ns = p2::monotonic_now_ns();
        const double end_to_end_ms =
            inference_end_ns >= frame.event.arrival_ns
            ? static_cast<double>(inference_end_ns - frame.event.arrival_ns) /
                  1.0e6
            : 0.0;
        ++stats.inference_frames;
        if (stats.inference_frames > options.warmup_frames) {
            if (stats.measured_frames == 0)
                measured_begin = std::chrono::steady_clock::now();
            measured_end = std::chrono::steady_clock::now();
            ++stats.measured_frames;
            if (!result.detections.empty())
                ++stats.frames_with_person;
            stats.person_detections += result.detections.size();
            stats.queue_age_ms.push_back(queue_age_ms);
            stats.rga_import_ms.push_back(result.timing.rga_import_ms);
            stats.rga_ms.push_back(result.timing.rga_ms);
            stats.rknn_ms.push_back(result.timing.rknn_ms);
            stats.post_ms.push_back(result.timing.postprocess_ms);
            stats.total_ms.push_back(result.timing.total_ms);
            stats.end_to_end_ms.push_back(end_to_end_ms);

            for (const p2::PersonDetection &detection : result.detections) {
                if (detection_csv) {
                    detection_csv << frame.event.sequence << ','
                                  << frame.event.timestamp_ns << ','
                                  << detection.confidence << ','
                                  << detection.box.left << ','
                                  << detection.box.top << ','
                                  << detection.box.right << ','
                                  << detection.box.bottom << ','
                                  << queue_age_ms << ','
                                  << result.timing.rga_import_ms << ','
                                  << result.timing.rga_ms << ','
                                  << result.timing.rknn_ms << ','
                                  << result.timing.postprocess_ms << ','
                                  << result.timing.total_ms << ','
                                  << end_to_end_ms << '\n';
                }
            }
        }

        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - run_begin).count();
        const bool reached_duration =
            elapsed >= static_cast<double>(options.duration_seconds);
        const bool reached_frame_limit = options.maximum_frames != 0 &&
            stats.measured_frames >= options.maximum_frames;
        lease.reset();
        if (g_stop.load() || reached_duration || reached_frame_limit) {
            g_stop.store(true);
            queue.close(true);
            break;
        }
    }
    g_stop.store(true);
    lease.reset();
    queue.close(true);
    capture_thread.join();
    const auto run_end = std::chrono::steady_clock::now();

    if (!capture_ok) {
        std::cerr << "visible capture failed: " << capture_error << '\n';
        return EXIT_FAILURE;
    }
    if (!inference_error.empty()) {
        std::cerr << "person inference failed: " << inference_error << '\n';
        return EXIT_FAILURE;
    }
    if (stats.measured_frames == 0) {
        std::cerr << "no measured inference frames were produced\n";
        return EXIT_FAILURE;
    }

    const double measured_elapsed_seconds = std::chrono::duration<double>(
        measured_end - measured_begin).count();
    const double run_elapsed_seconds = std::chrono::duration<double>(
        run_end - run_begin).count();
    const p2::BoundedQueueStats queue_stats = queue.stats();
    write_summary(std::cout, options, runtime, capture.stats(), queue_stats,
                  stats, measured_elapsed_seconds, run_elapsed_seconds);
    if (!options.json_output.empty()) {
        std::ofstream json(options.json_output);
        if (!json) {
            std::cerr << "cannot write summary JSON: " << options.json_output
                      << '\n';
            return EXIT_FAILURE;
        }
        write_summary(json, options, runtime, capture.stats(), queue_stats,
                      stats, measured_elapsed_seconds, run_elapsed_seconds);
    }
    return EXIT_SUCCESS;
}
