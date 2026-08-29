#include "p2/capture/device_discovery.hpp"
#include "p2/capture/visible_capture.hpp"
#include "p2/evaluation/person_dataset.hpp"
#include "p2/inference/person_detector.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>

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
    std::string output_directory = "/tmp/p2-person-dataset";
    std::uint64_t samples = 40;
    std::uint64_t interval_ms = 1000;
    std::uint64_t duration_seconds = 600;
    std::uint64_t warmup_frames = 10;
    std::uint32_t preview_long_edge = 960;
    p2::ImageRotation rotation = p2::ImageRotation::kClockwise270;
    float confidence_threshold = 0.25F;
    float nms_threshold = 0.45F;
};

struct CaptureStats {
    std::uint64_t visited_frames = 0;
    std::uint64_t sampled_frames = 0;
    std::uint64_t frames_with_person = 0;
    std::uint64_t predictions = 0;
    std::uint64_t preview_bytes = 0;
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
        << "  --output DIR         new dataset directory\n"
        << "  --samples N          number of sampled frames, default 40\n"
        << "  --interval-ms N      minimum spacing, default 1000\n"
        << "  --duration-sec N     safety timeout, default 600\n"
        << "  --warmup N           frames skipped before sampling, default 10\n"
        << "  --preview-edge N     BMP long edge, default 960\n"
        << "  --rotation VALUE     0, 90cw, 180 or 270cw; default 270cw\n"
        << "  --confidence VALUE   person threshold, default 0.25\n"
        << "  --nms VALUE          NMS IoU threshold, default 0.45\n"
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

bool parse_u32(const char *text, std::uint32_t *value)
{
    std::uint64_t parsed = 0;
    if (!parse_u64(text, &parsed) || parsed > UINT32_MAX)
        return false;
    *value = static_cast<std::uint32_t>(parsed);
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
        else if (argument == "--output")
            options->output_directory = value;
        else if (argument == "--samples") {
            if (!parse_u64(value, &options->samples))
                return false;
        } else if (argument == "--interval-ms") {
            if (!parse_u64(value, &options->interval_ms))
                return false;
        } else if (argument == "--duration-sec") {
            if (!parse_u64(value, &options->duration_seconds))
                return false;
        } else if (argument == "--warmup") {
            if (!parse_u64(value, &options->warmup_frames))
                return false;
        } else if (argument == "--preview-edge") {
            if (!parse_u32(value, &options->preview_long_edge))
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
    return !options->model.empty() && !options->output_directory.empty() &&
        options->samples > 0 && options->samples <= 10000U &&
        options->interval_ms >= 100U && options->interval_ms <= 600000U &&
        options->duration_seconds > 0 && options->duration_seconds <= 86400U &&
        options->warmup_frames <= 1000U &&
        options->preview_long_edge >= 160U &&
        options->preview_long_edge <= 2160U &&
        options->confidence_threshold > 0.0F &&
        options->confidence_threshold < 1.0F &&
        options->nms_threshold > 0.0F && options->nms_threshold < 1.0F;
}

bool path_exists(const std::string &path)
{
    struct stat status {};
    return ::stat(path.c_str(), &status) == 0;
}

bool ensure_directory(const std::string &path, std::string *error)
{
    if (path.empty()) {
        if (error != nullptr)
            *error = "directory path is empty";
        return false;
    }
    std::string current;
    std::size_t position = 0;
    if (path.front() == '/') {
        current = "/";
        position = 1;
    }
    while (position <= path.size()) {
        const std::size_t separator = path.find('/', position);
        const std::size_t length = separator == std::string::npos
            ? path.size() - position : separator - position;
        const std::string component = path.substr(position, length);
        if (!component.empty()) {
            if (!current.empty() && current.back() != '/')
                current.push_back('/');
            current += component;
            if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                if (error != nullptr)
                    *error = "cannot create directory " + current + ": " +
                        std::strerror(errno);
                return false;
            }
        }
        if (separator == std::string::npos)
            break;
        position = separator + 1U;
    }
    return true;
}

std::string join_path(const std::string &directory, const std::string &name)
{
    return directory + (directory.empty() || directory.back() == '/'
                            ? "" : "/") + name;
}

std::string image_id(std::uint64_t sample_index)
{
    std::ostringstream output;
    output << "frame-" << std::setw(6) << std::setfill('0') << sample_index;
    return output.str();
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

void write_summary(std::ostream &output,
                   const Options &options,
                   const std::string &visible_device,
                   const p2::PersonDetectorRuntimeInfo &runtime,
                   const p2::VisibleCaptureStats &capture,
                   const CaptureStats &stats,
                   double elapsed_seconds,
                   bool complete)
{
    output << std::fixed << std::setprecision(3)
           << "{\n"
           << "  \"schema\": \"p2.person-dataset-capture.v1\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT << "\",\n"
           << "  \"complete\": " << (complete ? "true" : "false") << ",\n"
           << "  \"visible_device\": \"" << json_escape(visible_device)
           << "\",\n"
           << "  \"model_path\": \"" << json_escape(options.model) << "\",\n"
           << "  \"model_sha256_expected\": \"" << kModelSha256 << "\",\n"
           << "  \"rotation\": \""
           << p2::image_rotation_name(options.rotation) << "\",\n"
           << "  \"confidence_threshold\": "
           << options.confidence_threshold << ",\n"
           << "  \"nms_threshold\": " << options.nms_threshold << ",\n"
           << "  \"sample_interval_ms\": " << options.interval_ms << ",\n"
           << "  \"requested_samples\": " << options.samples << ",\n"
           << "  \"sampled_frames\": " << stats.sampled_frames << ",\n"
           << "  \"frames_with_person_prediction\": "
           << stats.frames_with_person << ",\n"
           << "  \"person_predictions\": " << stats.predictions << ",\n"
           << "  \"preview_bytes\": " << stats.preview_bytes << ",\n"
           << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n"
           << "  \"rknn_api_version\": \""
           << json_escape(runtime.rknn_api_version) << "\",\n"
           << "  \"rknn_driver_version\": \""
           << json_escape(runtime.rknn_driver_version) << "\",\n"
           << "  \"capture\": {\"width\": " << capture.width
           << ", \"height\": " << capture.height
           << ", \"bytes_per_line\": " << capture.bytes_per_line
           << ", \"dqbuf_count\": " << capture.dqbuf_count
           << ", \"sequence_gaps\": " << capture.sequence_gaps
           << ", \"bad_bytes_used\": " << capture.bad_bytes_used
           << ", \"missing_monotonic_timestamp\": "
           << capture.missing_monotonic_timestamp << "}\n"
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

    std::string media_device;
    std::string error;
    if (options.visible.empty() &&
        !p2::discover_visible_device(&options.visible, &media_device, &error)) {
        std::cerr << "visible discovery failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    const std::string manifest_path =
        join_path(options.output_directory, "manifest.csv");
    if (path_exists(manifest_path)) {
        std::cerr << "refusing to overwrite existing dataset: "
                  << manifest_path << '\n';
        return EXIT_FAILURE;
    }
    const std::string images_directory =
        join_path(options.output_directory, "images");
    if (!ensure_directory(images_directory, &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    std::ofstream manifest(manifest_path);
    std::ofstream predictions(
        join_path(options.output_directory, "predictions.csv"));
    std::ofstream annotations(
        join_path(options.output_directory, "annotations.csv"));
    if (!manifest || !predictions || !annotations) {
        std::cerr << "cannot create dataset CSV files\n";
        return EXIT_FAILURE;
    }
    manifest << "image_id,image_file,sequence,timestamp_ns,source_width,"
                "source_height,preview_width,preview_height,rotation\n";
    predictions << "image_id,confidence,left,top,right,bottom\n";
    annotations << "image_id,left,top,right,bottom\n";

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

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    CaptureStats stats;
    p2::PersonPreviewGeometry geometry;
    bool geometry_ready = false;
    std::uint64_t next_sample_timestamp_ns = 0;
    std::string runtime_error;
    const auto run_begin = std::chrono::steady_clock::now();
    p2::VisibleCapture capture({options.visible});
    std::cout << "P2.3.2 dataset capture: output="
              << options.output_directory << " samples=" << options.samples
              << " interval_ms=" << options.interval_ms << " rotation="
              << p2::image_rotation_name(options.rotation) << '\n';

    const bool capture_ok = capture.run_frames(
        g_stop,
        [&](const p2::VisibleFrameView &frame) {
            ++stats.visited_frames;
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - run_begin).count();
            if (elapsed >= static_cast<double>(options.duration_seconds)) {
                g_stop.store(true);
                return;
            }
            if (stats.visited_frames <= options.warmup_frames)
                return;
            if (next_sample_timestamp_ns != 0 &&
                frame.event.timestamp_ns < next_sample_timestamp_ns)
                return;

            if (!geometry_ready) {
                if (!p2::make_person_preview_geometry(
                        frame.width, frame.height, options.preview_long_edge,
                        options.rotation, &geometry, &runtime_error)) {
                    g_stop.store(true);
                    return;
                }
                geometry_ready = true;
            } else if (frame.width != geometry.source_width ||
                       frame.height != geometry.source_height) {
                runtime_error = "capture dimensions changed during dataset";
                g_stop.store(true);
                return;
            }

            p2::PersonInferenceResult inference;
            if (!detector.infer_nv12(frame.data, frame.size, frame.width,
                                     frame.height, frame.bytes_per_line,
                                     &inference, &runtime_error)) {
                g_stop.store(true);
                return;
            }
            const std::uint64_t sample_index = stats.sampled_frames + 1U;
            const std::string id = image_id(sample_index);
            const std::string file_name = id + ".bmp";
            const std::string image_path = join_path(images_directory,
                                                     file_name);
            if (!p2::write_nv12_person_preview_bmp(
                    image_path, frame.data, frame.size, frame.bytes_per_line,
                    geometry, &runtime_error)) {
                g_stop.store(true);
                return;
            }

            manifest << id << ",images/" << file_name << ','
                     << frame.event.sequence << ',' << frame.event.timestamp_ns
                     << ',' << frame.width << ',' << frame.height << ','
                     << geometry.preview_width << ',' << geometry.preview_height
                     << ',' << p2::image_rotation_name(options.rotation)
                     << '\n';
            for (const p2::PersonDetection &detection :
                 inference.detections) {
                const p2::FloatBox box = p2::source_to_person_preview_box(
                    geometry, detection.box);
                predictions << id << ',' << std::fixed << std::setprecision(6)
                            << detection.confidence << ',' << box.left << ','
                            << box.top << ',' << box.right << ',' << box.bottom
                            << '\n';
            }
            manifest.flush();
            predictions.flush();
            ++stats.sampled_frames;
            if (!inference.detections.empty())
                ++stats.frames_with_person;
            stats.predictions += inference.detections.size();
            struct stat image_status {};
            if (::stat(image_path.c_str(), &image_status) == 0)
                stats.preview_bytes +=
                    static_cast<std::uint64_t>(image_status.st_size);
            std::cout << "sample " << stats.sampled_frames << '/'
                      << options.samples << " sequence="
                      << frame.event.sequence << " predictions="
                      << inference.detections.size() << '\n';

            next_sample_timestamp_ns = frame.event.timestamp_ns +
                options.interval_ms * 1000000ULL;
            if (stats.sampled_frames >= options.samples)
                g_stop.store(true);
        },
        &error);
    const double elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - run_begin).count();

    if (!capture_ok) {
        std::cerr << "visible capture failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    if (!runtime_error.empty()) {
        std::cerr << "dataset capture failed: " << runtime_error << '\n';
        return EXIT_FAILURE;
    }
    const bool complete = stats.sampled_frames == options.samples;
    const p2::PersonDetectorRuntimeInfo &runtime = detector.runtime_info();
    std::ofstream summary(
        join_path(options.output_directory, "capture-summary.json"));
    if (!summary) {
        std::cerr << "cannot write capture summary\n";
        return EXIT_FAILURE;
    }
    write_summary(summary, options, options.visible, runtime, capture.stats(),
                  stats, elapsed_seconds, complete);
    write_summary(std::cout, options, options.visible, runtime,
                  capture.stats(), stats, elapsed_seconds, complete);
    if (!complete) {
        std::cerr << "dataset stopped before requested sample count\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
