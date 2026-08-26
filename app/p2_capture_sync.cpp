#include "p2/capture/device_discovery.hpp"
#include "p2/capture/thermal_capture.hpp"
#include "p2/capture/v4l2_utils.hpp"
#include "p2/capture/visible_capture.hpp"
#include "p2/sync/event_queue.hpp"
#include "p2/sync/frame_synchronizer.hpp"

#include <algorithm>
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
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

constexpr const char *kProjectOneCommit =
    "c140532b82723bdfe1396b51304f2dfe9c4c73e7";
constexpr const char *kZmlxHeaderSha256 =
    "241058cbb9c56224a2b055134302bc85776c2b06dd7e375c85f3c03e5a54cea4";

volatile std::sig_atomic_t g_signal_received = 0;

void handle_signal(int signal_number)
{
    g_signal_received = signal_number;
}

struct Options {
    std::string visible;
    std::string thermal;
    std::string output = "/tmp/p2-stage1";
    std::uint64_t duration_seconds = 60;
    std::uint64_t max_skew_ms = 25;
    std::uint64_t lookahead_ms = 20;
    std::uint64_t consumer_delay_ms = 0;
    std::uint64_t queue_capacity = 128;
};

class RunState {
public:
    void fail(const std::string &message)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (error_.empty())
                error_ = message;
        }
        stop.store(true);
    }

    std::string error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    std::atomic<bool> stop{false};

private:
    mutable std::mutex mutex_;
    std::string error_;
};

void print_usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --visible PATH       override dynamically discovered mainpath\n"
        << "  --thermal PATH       override dynamically discovered ZMLX node\n"
        << "  --duration SEC       capture duration, default 60\n"
        << "  --output DIR         report directory, default /tmp/p2-stage1\n"
        << "  --max-skew-ms MS     reject pairs beyond this skew; 0 disables\n"
        << "  --lookahead-ms MS    wait for future visible timestamps\n"
        << "  --consumer-delay-ms  delay sync consumer for slow-consumer testing\n"
        << "  --queue-capacity N    bounded producer queue capacity, default 128\n"
        << "  --help               show this text\n";
}

bool parse_unsigned(const char *text, std::uint64_t *value)
{
    if (text == nullptr || value == nullptr || *text == '\0')
        return false;
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
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (index + 1 >= argc) {
            std::cerr << "missing value for " << argument << '\n';
            return false;
        }
        const char *value = argv[++index];
        if (argument == "--visible")
            options->visible = value;
        else if (argument == "--thermal")
            options->thermal = value;
        else if (argument == "--output")
            options->output = value;
        else if (argument == "--duration") {
            if (!parse_unsigned(value, &options->duration_seconds) ||
                options->duration_seconds == 0)
                return false;
        } else if (argument == "--max-skew-ms") {
            if (!parse_unsigned(value, &options->max_skew_ms))
                return false;
        } else if (argument == "--lookahead-ms") {
            if (!parse_unsigned(value, &options->lookahead_ms))
                return false;
        } else if (argument == "--consumer-delay-ms") {
            if (!parse_unsigned(value, &options->consumer_delay_ms))
                return false;
        } else if (argument == "--queue-capacity") {
            if (!parse_unsigned(value, &options->queue_capacity) ||
                options->queue_capacity == 0)
                return false;
        } else {
            std::cerr << "unknown option: " << argument << '\n';
            return false;
        }
    }
    return !options->output.empty();
}

bool make_directories(const std::string &path)
{
    if (path.empty())
        return false;
    std::string current;
    if (path.front() == '/')
        current = "/";
    std::size_t start = path.front() == '/' ? 1 : 0;
    while (start <= path.size()) {
        const std::size_t separator = path.find('/', start);
        const std::string component = path.substr(
            start, separator == std::string::npos ? std::string::npos
                                                   : separator - start);
        if (!component.empty()) {
            if (!current.empty() && current.back() != '/')
                current += '/';
            current += component;
            if (mkdir(current.c_str(), 0755) < 0 && errno != EEXIST)
                return false;
        }
        if (separator == std::string::npos)
            break;
        start = separator + 1;
    }
    return true;
}

double percentile_ms(std::vector<std::uint64_t> values, double percentile)
{
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const double rank = std::ceil(percentile * values.size());
    std::size_t index = rank > 0.0 ? static_cast<std::size_t>(rank - 1.0) : 0;
    index = std::min(index, values.size() - 1);
    return static_cast<double>(values[index]) / 1'000'000.0;
}

bool write_csv(const std::string &path,
               const std::vector<p2::MatchRecord> &matches)
{
    std::ofstream output(path);
    if (!output)
        return false;
    output << "visible_sequence,thermal_sequence,thermal_pair_sequence,"
              "visible_timestamp_ns,thermal_timestamp_ns,signed_delta_ns,"
              "absolute_delta_ns,thermal_span_ns,visible_arrival_ns,"
              "thermal_arrival_ns\n";
    for (const p2::MatchRecord &match : matches) {
        output << match.visible_sequence << ',' << match.thermal_sequence << ','
               << match.thermal_pair_sequence << ','
               << match.visible_timestamp_ns << ','
               << match.thermal_timestamp_ns << ',' << match.signed_delta_ns
               << ',' << match.absolute_delta_ns << ','
               << match.thermal_span_ns << ',' << match.visible_arrival_ns
               << ',' << match.thermal_arrival_ns << '\n';
    }
    return output.good();
}

bool write_summary(const std::string &path, const Options &options,
                   const p2::DiscoveredDevices &devices,
                   const p2::VisibleCaptureStats &visible,
                   const p2::ThermalCaptureStats &thermal,
                   const p2::SynchronizerStats &sync,
                   const p2::EventQueueStats &event_queue,
                   const std::vector<p2::MatchRecord> &matches,
                   std::uint64_t elapsed_ms, bool passed,
                   const std::string &error)
{
    std::vector<std::uint64_t> deltas;
    deltas.reserve(matches.size());
    for (const p2::MatchRecord &match : matches)
        deltas.push_back(match.absolute_delta_ns);

    std::ofstream output(path);
    if (!output)
        return false;
    output << std::fixed << std::setprecision(3);
    output << "{\n"
           << "  \"schema\": \"p2.capture-sync.v1\",\n"
           << "  \"result\": \"" << (passed ? "PASS" : "FAIL") << "\",\n"
           << "  \"error\": \"" << error << "\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT << "\",\n"
           << "  \"project_one_commit\": \"" << kProjectOneCommit << "\",\n"
           << "  \"zmlx_header_sha256\": \"" << kZmlxHeaderSha256 << "\",\n"
           << "  \"visible_device\": \"" << devices.visible << "\",\n"
           << "  \"visible_media\": \"" << devices.visible_media << "\",\n"
           << "  \"thermal_device\": \"" << devices.thermal << "\",\n"
           << "  \"requested_duration_s\": " << options.duration_seconds << ",\n"
           << "  \"elapsed_ms\": " << elapsed_ms << ",\n"
           << "  \"max_skew_ms\": " << options.max_skew_ms << ",\n"
           << "  \"lookahead_ms\": " << options.lookahead_ms << ",\n"
           << "  \"consumer_delay_ms\": " << options.consumer_delay_ms << ",\n"
           << "  \"event_queue_capacity\": " << options.queue_capacity << ",\n"
           << "  \"visible\": {\n"
           << "    \"frames\": " << visible.dqbuf_count << ",\n"
           << "    \"sequence_first\": " << visible.first_sequence << ",\n"
           << "    \"sequence_last\": " << visible.last_sequence << ",\n"
           << "    \"sequence_gaps\": " << visible.sequence_gaps << ",\n"
           << "    \"bad_bytes_used\": " << visible.bad_bytes_used << ",\n"
           << "    \"missing_monotonic\": "
           << visible.missing_monotonic_timestamp << ",\n"
           << "    \"width\": " << visible.width << ",\n"
           << "    \"height\": " << visible.height << ",\n"
           << "    \"bytes_per_line\": " << visible.bytes_per_line << ",\n"
           << "    \"size_image\": " << visible.size_image << ",\n"
           << "    \"buffers\": " << visible.allocated_buffers << "\n"
           << "  },\n"
           << "  \"thermal\": {\n"
           << "    \"dqbuf\": " << thermal.dqbuf_count << ",\n"
           << "    \"valid_pairs\": " << thermal.valid_pairs << ",\n"
           << "    \"sequence_gaps\": " << thermal.sequence_gaps << ",\n"
           << "    \"invalid_payloads\": " << thermal.invalid_payloads << ",\n"
           << "    \"missing_monotonic\": "
           << thermal.missing_monotonic_timestamp << ",\n"
           << "    \"timestamp_mismatches\": "
           << thermal.timestamp_mismatches << ",\n"
           << "    \"buffer_size\": " << thermal.buffer_size << ",\n"
           << "    \"buffers\": " << thermal.allocated_buffers << "\n"
           << "  },\n"
           << "  \"sync\": {\n"
           << "    \"matched\": " << sync.matched << ",\n"
           << "    \"unmatched_skew\": " << sync.unmatched_skew << ",\n"
           << "    \"unmatched_no_visible\": "
           << sync.unmatched_no_visible << ",\n"
           << "    \"thermal_queue_drops\": "
           << sync.thermal_queue_drops << ",\n"
           << "    \"visible_history_pruned\": "
           << sync.visible_history_pruned << ",\n"
           << "    \"visible_high_watermark\": "
           << sync.visible_high_watermark << ",\n"
           << "    \"thermal_high_watermark\": "
           << sync.thermal_high_watermark << ",\n"
           << "    \"delta_p50_ms\": " << percentile_ms(deltas, 0.50) << ",\n"
           << "    \"delta_p95_ms\": " << percentile_ms(deltas, 0.95) << ",\n"
           << "    \"delta_p99_ms\": " << percentile_ms(deltas, 0.99) << ",\n"
           << "    \"delta_max_ms\": " << percentile_ms(deltas, 1.00) << "\n"
           << "  },\n"
           << "  \"event_queue\": {\n"
           << "    \"pushed\": " << event_queue.pushed << ",\n"
           << "    \"popped\": " << event_queue.popped << ",\n"
           << "    \"visible_drops\": " << event_queue.visible_drops << ",\n"
           << "    \"thermal_drops\": " << event_queue.thermal_drops << ",\n"
           << "    \"high_watermark\": " << event_queue.high_watermark << ",\n"
           << "    \"pending\": " << event_queue.pending << "\n"
           << "  }\n"
           << "}\n";
    return output.good();
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (!make_directories(options.output)) {
        std::cerr << "failed to create output directory " << options.output
                  << ": " << std::strerror(errno) << '\n';
        return EXIT_FAILURE;
    }

    p2::DiscoveredDevices devices;
    std::string discovery_error;
    if (options.visible.empty()) {
        if (!p2::discover_visible_device(&devices.visible,
                                         &devices.visible_media,
                                         &discovery_error)) {
            std::cerr << "visible discovery failed: " << discovery_error << '\n';
            return EXIT_FAILURE;
        }
    } else {
        devices.visible = options.visible;
        devices.visible_media = "manual-override";
    }
    if (options.thermal.empty()) {
        if (!p2::discover_thermal_device(&devices.thermal,
                                         &discovery_error)) {
            std::cerr << "thermal discovery failed: " << discovery_error << '\n';
            return EXIT_FAILURE;
        }
    } else {
        devices.thermal = options.thermal;
    }

    std::cout << "VISIBLE_DEVICE=" << devices.visible << '\n'
              << "VISIBLE_MEDIA=" << devices.visible_media << '\n'
              << "THERMAL_DEVICE=" << devices.thermal << '\n';

    p2::SynchronizerConfig sync_config;
    sync_config.max_skew_ns = options.max_skew_ms * 1'000'000ULL;
    sync_config.lookahead_ns = options.lookahead_ms * 1'000'000ULL;
    p2::FrameSynchronizer synchronizer(sync_config);
    p2::BoundedEventQueue event_queue(
        static_cast<std::size_t>(options.queue_capacity));

    p2::VisibleCaptureConfig visible_config;
    visible_config.device = devices.visible;
    p2::ThermalCaptureConfig thermal_config;
    thermal_config.device = devices.thermal;
    p2::VisibleCapture visible_capture(visible_config);
    p2::ThermalCapture thermal_capture(thermal_config);

    RunState run_state;
    bool visible_result = false;
    bool thermal_result = false;
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    const std::uint64_t start_ns = p2::monotonic_now_ns();

    std::thread visible_thread([&]() {
        std::string error;
        visible_result = visible_capture.run(
            run_state.stop,
            [&](const p2::VisibleFrameEvent &frame) {
                event_queue.push(frame);
            },
            &error);
        if (!visible_result && !run_state.stop.load())
            run_state.fail("visible: " + error);
    });
    std::thread thermal_thread([&]() {
        std::string error;
        thermal_result = thermal_capture.run(
            run_state.stop,
            [&](const p2::ThermalFrameEvent &frame) {
                event_queue.push(frame);
            },
            &error);
        if (!thermal_result && !run_state.stop.load())
            run_state.fail("thermal: " + error);
    });

    std::thread sync_thread([&]() {
        p2::FrameEvent event;
        while (event_queue.wait_pop(&event)) {
            if (options.consumer_delay_ms != 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    options.consumer_delay_ms));
            std::visit(
                [&](const auto &frame) {
                    using Frame = std::decay_t<decltype(frame)>;
                    if constexpr (std::is_same_v<Frame,
                                                 p2::VisibleFrameEvent>)
                        synchronizer.push_visible(frame);
                    else
                        synchronizer.push_thermal(frame);
                },
                event);
        }
    });

    const std::uint64_t duration_ns =
        options.duration_seconds * 1'000'000'000ULL;
    while (!run_state.stop.load()) {
        if (g_signal_received != 0 ||
            p2::monotonic_now_ns() - start_ns >= duration_ns) {
            run_state.stop.store(true);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    visible_thread.join();
    thermal_thread.join();
    event_queue.close();
    sync_thread.join();
    synchronizer.flush();

    const std::uint64_t elapsed_ms =
        (p2::monotonic_now_ns() - start_ns) / 1'000'000ULL;
    const auto visible_stats = visible_capture.stats();
    const auto thermal_stats = thermal_capture.stats();
    const auto sync_stats = synchronizer.stats();
    const auto event_queue_stats = event_queue.stats();
    const auto matches = synchronizer.matches();
    const std::string capture_error = run_state.error();
    const bool passed = capture_error.empty() && visible_result &&
        thermal_result && visible_stats.dqbuf_count > 0 &&
        thermal_stats.valid_pairs > 0 && !matches.empty() &&
        visible_stats.bad_bytes_used == 0 &&
        visible_stats.missing_monotonic_timestamp == 0 &&
        thermal_stats.invalid_payloads == 0 &&
        thermal_stats.missing_monotonic_timestamp == 0 &&
        thermal_stats.timestamp_mismatches == 0;

    const std::string csv_path = options.output + "/pairs.csv";
    const std::string summary_path = options.output + "/summary.json";
    if (!write_csv(csv_path, matches) ||
        !write_summary(summary_path, options, devices, visible_stats,
                       thermal_stats, sync_stats, event_queue_stats, matches,
                       elapsed_ms,
                       passed, capture_error)) {
        std::cerr << "failed to write reports under " << options.output << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "VISIBLE_FRAMES=" << visible_stats.dqbuf_count << '\n'
              << "THERMAL_PAIRS=" << thermal_stats.valid_pairs << '\n'
              << "MATCHED=" << sync_stats.matched << '\n'
              << "UNMATCHED_SKEW=" << sync_stats.unmatched_skew << '\n'
              << "EVENT_QUEUE_DROPS="
              << event_queue_stats.visible_drops + event_queue_stats.thermal_drops
              << '\n'
              << "REPORT=" << summary_path << '\n'
              << "P2_STAGE1_CAPTURE_SYNC=" << (passed ? "PASS" : "FAIL")
              << '\n';
    if (!capture_error.empty())
        std::cerr << "capture error: " << capture_error << '\n';
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
