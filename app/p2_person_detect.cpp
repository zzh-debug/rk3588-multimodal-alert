#include "p2/capture/device_discovery.hpp"
#include "p2/capture/visible_capture.hpp"
#include "p2/inference/person_detector.hpp"

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
#include <limits>
#include <string>
#include <vector>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

constexpr const char *kModelSha256 =
    "7c6b801de602b8aaa72269fab8daab21810418a4b01c8e8995313916e278fe71";

std::atomic<bool> g_stop{false};

struct Options {
    std::string visible;
    std::string model = "/usr/share/p2/models/yolov5s-640-640.rknn";
    std::string json_output;
    std::string detection_csv;
    std::uint64_t duration_seconds = 15;
    std::uint64_t maximum_frames = 0;
    std::uint64_t warmup_frames = 10;
    p2::ImageRotation rotation = p2::ImageRotation::kClockwise90;
    float confidence_threshold = 0.25F;
    float nms_threshold = 0.45F;
};

struct RunStats {
    std::uint64_t inference_frames = 0;
    std::uint64_t measured_frames = 0;
    std::uint64_t frames_with_person = 0;
    std::uint64_t person_detections = 0;
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
        << "  --duration-sec N     run time, default 15\n"
        << "  --frames N           measured frame limit, 0 means duration only\n"
        << "  --warmup N           warm-up frames excluded from timing, default 10\n"
        << "  --rotation VALUE     0, 90cw, 180 or 270cw; default 90cw\n"
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
    return !options->model.empty() && options->duration_seconds <= 86400U &&
        options->warmup_frames <= 1000U &&
        options->confidence_threshold > 0.0F &&
        options->confidence_threshold < 1.0F && options->nms_threshold > 0.0F &&
        options->nms_threshold < 1.0F;
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
                   const std::string &visible_device,
                   const p2::PersonDetectorRuntimeInfo &runtime,
                   const p2::VisibleCaptureStats &capture,
                   const RunStats &stats,
                   double measured_elapsed_seconds)
{
    output << std::fixed << std::setprecision(3);
    output << "{\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT << "\",\n"
           << "  \"visible_device\": \"" << json_escape(visible_device)
           << "\",\n"
           << "  \"model_path\": \"" << json_escape(options.model) << "\",\n"
           << "  \"model_sha256_expected\": \"" << kModelSha256 << "\",\n"
           << "  \"rotation\": \""
           << p2::image_rotation_name(options.rotation) << "\",\n"
           << "  \"rknn_api_version\": \""
           << json_escape(runtime.rknn_api_version) << "\",\n"
           << "  \"rknn_driver_version\": \""
           << json_escape(runtime.rknn_driver_version) << "\",\n"
           << "  \"model_input\": {\"width\": " << runtime.model_width
           << ", \"height\": " << runtime.model_height
           << ", \"channels\": " << runtime.model_channels << "},\n"
           << "  \"capture\": {\"width\": " << capture.width
           << ", \"height\": " << capture.height
           << ", \"bytes_per_line\": " << capture.bytes_per_line
           << ", \"dqbuf_count\": " << capture.dqbuf_count
           << ", \"sequence_gaps\": " << capture.sequence_gaps
           << ", \"bad_bytes_used\": " << capture.bad_bytes_used
           << ", \"missing_monotonic_timestamp\": "
           << capture.missing_monotonic_timestamp << "},\n"
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
           << "  \"latency\": {\n";
    write_latency_json(output, "rga", stats.rga_ms, true);
    write_latency_json(output, "rknn", stats.rknn_ms, true);
    write_latency_json(output, "postprocess", stats.post_ms, true);
    write_latency_json(output, "total", stats.total_ms, false);
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
    p2::RknnPersonDetector detector(detector_config);
    if (!detector.initialize(&error)) {
        std::cerr << "detector initialization failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    const p2::PersonDetectorRuntimeInfo &runtime = detector.runtime_info();
    std::cout << "P2.3 person detector: visible=" << options.visible
              << " model=" << options.model
              << " rotation=" << p2::image_rotation_name(options.rotation)
              << " RKNN=" << runtime.rknn_api_version
              << " driver=" << runtime.rknn_driver_version << '\n';

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
               "rga_ms,rknn_ms,postprocess_ms,total_ms\n";
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    RunStats stats;
    std::string runtime_error;
    const auto run_begin = std::chrono::steady_clock::now();
    auto measured_begin = run_begin;
    p2::VisibleCapture capture({options.visible});
    const bool capture_ok = capture.run_frames(
        g_stop,
        [&](const p2::VisibleFrameView &frame) {
            p2::PersonInferenceResult result;
            if (!detector.infer_nv12(frame.data, frame.size, frame.width,
                                     frame.height, frame.bytes_per_line,
                                     &result, &runtime_error)) {
                g_stop.store(true);
                return;
            }
            ++stats.inference_frames;
            if (stats.inference_frames <= options.warmup_frames)
                return;
            if (stats.measured_frames == 0)
                measured_begin = std::chrono::steady_clock::now();
            ++stats.measured_frames;
            if (!result.detections.empty())
                ++stats.frames_with_person;
            stats.person_detections += result.detections.size();
            stats.rga_ms.push_back(result.timing.rga_ms);
            stats.rknn_ms.push_back(result.timing.rknn_ms);
            stats.post_ms.push_back(result.timing.postprocess_ms);
            stats.total_ms.push_back(result.timing.total_ms);

            for (const p2::PersonDetection &detection : result.detections) {
                if (detection_csv) {
                    detection_csv << frame.event.sequence << ','
                                  << frame.event.timestamp_ns << ','
                                  << detection.confidence << ','
                                  << detection.box.left << ','
                                  << detection.box.top << ','
                                  << detection.box.right << ','
                                  << detection.box.bottom << ','
                                  << result.timing.rga_ms << ','
                                  << result.timing.rknn_ms << ','
                                  << result.timing.postprocess_ms << ','
                                  << result.timing.total_ms << '\n';
                }
            }
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - run_begin).count();
            if (elapsed >= static_cast<double>(options.duration_seconds) ||
                (options.maximum_frames != 0 &&
                 stats.measured_frames >= options.maximum_frames))
                g_stop.store(true);
        },
        &error);
    const auto run_end = std::chrono::steady_clock::now();

    if (!capture_ok) {
        std::cerr << "visible capture failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    if (!runtime_error.empty()) {
        std::cerr << "person inference failed: " << runtime_error << '\n';
        return EXIT_FAILURE;
    }
    if (stats.measured_frames == 0) {
        std::cerr << "no measured inference frames were produced\n";
        return EXIT_FAILURE;
    }
    const double measured_elapsed_seconds = std::chrono::duration<double>(
        run_end - measured_begin).count();
    write_summary(std::cout, options, options.visible, runtime,
                  capture.stats(), stats, measured_elapsed_seconds);
    if (!options.json_output.empty()) {
        std::ofstream json(options.json_output);
        if (!json) {
            std::cerr << "cannot write summary JSON: " << options.json_output
                      << '\n';
            return EXIT_FAILURE;
        }
        write_summary(json, options, options.visible, runtime,
                      capture.stats(), stats, measured_elapsed_seconds);
    }
    return EXIT_SUCCESS;
}
