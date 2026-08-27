#include "p2/capture/device_discovery.hpp"
#include "p2/capture/thermal_capture.hpp"
#include "p2/thermal/mlx90640_math.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

constexpr const char *kProjectOneCommit =
    "c140532b82723bdfe1396b51304f2dfe9c4c73e7";
constexpr const char *kEepromSha256 =
    "cc4628409b57ca3137065b10fd40c684fcef9d16ab493ace882710ad490185b2";

struct Options {
    std::string eeprom =
        "/sys/bus/nvmem/devices/zzh_mlx90640_eeprom-5-33/nvmem";
    std::string thermal;
    std::string output = "/tmp/p2-thermal-realtime.json";
    std::uint64_t requested_pairs = 600;
    float emissivity = 0.95F;
    float reflected_offset_c = -8.0F;
};

struct RealtimeStats {
    std::uint64_t pairs = 0;
    std::uint64_t math_failures = 0;
    double ta_sum = 0.0;
    float ta_min = std::numeric_limits<float>::infinity();
    float ta_max = -std::numeric_limits<float>::infinity();
    float scene_min = std::numeric_limits<float>::infinity();
    float scene_max = -std::numeric_limits<float>::infinity();
    float max_interframe_pixel_delta_c = 0.0F;
    std::uint64_t calculation_time_sum_ns = 0;
    std::uint64_t calculation_time_max_ns = 0;
    std::uint64_t subpage_span_min_ns =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t subpage_span_max_ns = 0;
    std::array<float, p2::kMlx90640Pixels> previous_temperature{};
    bool have_previous = false;
};

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --eeprom FILE              NVMEM EEPROM path\n"
        << "  --thermal PATH             override discovered ZMLX node\n"
        << "  --pairs N                  complete pairs, default 600\n"
        << "  --output FILE              JSON report path\n"
        << "  --emissivity VALUE         default 0.95\n"
        << "  --reflected-offset-c VALUE default -8.0\n";
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
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *value = parsed;
    return true;
}

bool parse(int argc, char **argv, Options *options)
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
        if (argument == "--eeprom")
            options->eeprom = value;
        else if (argument == "--thermal")
            options->thermal = value;
        else if (argument == "--output")
            options->output = value;
        else if (argument == "--pairs") {
            if (!parse_u64(value, &options->requested_pairs) ||
                options->requested_pairs == 0)
                return false;
        } else if (argument == "--emissivity") {
            if (!parse_float(value, &options->emissivity))
                return false;
        } else if (argument == "--reflected-offset-c") {
            if (!parse_float(value, &options->reflected_offset_c))
                return false;
        } else {
            return false;
        }
    }
    return !options->eeprom.empty() && !options->output.empty();
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

void update_stats(const p2::ThermalMathResult &result,
                  RealtimeStats *stats)
{
    ++stats->pairs;
    stats->ta_sum += result.ta_c;
    stats->ta_min = std::min(stats->ta_min, result.ta_c);
    stats->ta_max = std::max(stats->ta_max, result.ta_c);
    stats->scene_min = std::min(stats->scene_min,
                                result.min_temperature_c);
    stats->scene_max = std::max(stats->scene_max,
                                result.max_temperature_c);
    stats->calculation_time_sum_ns += result.calculation_time_ns;
    stats->calculation_time_max_ns = std::max(
        stats->calculation_time_max_ns, result.calculation_time_ns);
    stats->subpage_span_min_ns = std::min(
        stats->subpage_span_min_ns, result.subpage_span_ns);
    stats->subpage_span_max_ns = std::max(
        stats->subpage_span_max_ns, result.subpage_span_ns);
    if (stats->have_previous) {
        for (std::size_t index = 0; index < p2::kMlx90640Pixels; ++index) {
            stats->max_interframe_pixel_delta_c = std::max(
                stats->max_interframe_pixel_delta_c,
                std::fabs(result.temperature_c[index] -
                          stats->previous_temperature[index]));
        }
    }
    stats->previous_temperature = result.temperature_c;
    stats->have_previous = true;
}

void write_report(std::ostream &output, const Options &options,
                  const RealtimeStats &stats,
                  const p2::ThermalCaptureStats &capture,
                  std::uint64_t elapsed_ms, bool passed,
                  const std::string &error)
{
    const double ta_mean = stats.pairs == 0
        ? 0.0
        : stats.ta_sum / static_cast<double>(stats.pairs);
    const std::uint64_t calculation_mean_ns = stats.pairs == 0
        ? 0
        : stats.calculation_time_sum_ns / stats.pairs;
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"schema\": \"p2.thermal-realtime.v1\",\n"
           << "  \"result\": \"" << (passed ? "PASS" : "FAIL")
           << "\",\n"
           << "  \"error\": \"" << error << "\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT
           << "\",\n"
           << "  \"project_one_commit\": \"" << kProjectOneCommit
           << "\",\n"
           << "  \"eeprom_sha256\": \"" << kEepromSha256 << "\",\n"
           << "  \"thermal_device\": \"" << options.thermal << "\",\n"
           << "  \"requested_pairs\": " << options.requested_pairs
           << ",\n"
           << "  \"processed_pairs\": " << stats.pairs << ",\n"
           << "  \"elapsed_ms\": " << elapsed_ms << ",\n"
           << "  \"emissivity\": " << options.emissivity << ",\n"
           << "  \"reflected_temperature_offset_c\": "
           << options.reflected_offset_c << ",\n"
           << "  \"ta_mean_c\": " << ta_mean << ",\n"
           << "  \"ta_min_c\": " << stats.ta_min << ",\n"
           << "  \"ta_max_c\": " << stats.ta_max << ",\n"
           << "  \"scene_min_c\": " << stats.scene_min << ",\n"
           << "  \"scene_max_c\": " << stats.scene_max << ",\n"
           << "  \"max_interframe_pixel_delta_c\": "
           << stats.max_interframe_pixel_delta_c << ",\n"
           << "  \"calculation_mean_ns\": " << calculation_mean_ns
           << ",\n"
           << "  \"calculation_max_ns\": "
           << stats.calculation_time_max_ns << ",\n"
           << "  \"subpage_span_min_ns\": "
           << stats.subpage_span_min_ns << ",\n"
           << "  \"subpage_span_max_ns\": "
           << stats.subpage_span_max_ns << ",\n"
           << "  \"math_failures\": " << stats.math_failures << ",\n"
           << "  \"capture_invalid_payloads\": "
           << capture.invalid_payloads << ",\n"
           << "  \"capture_sequence_gaps\": "
           << capture.sequence_gaps << ",\n"
           << "  \"capture_timestamp_mismatches\": "
           << capture.timestamp_mismatches << "\n"
           << "}\n";
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse(argc, argv, &options)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (options.thermal.empty()) {
        std::string discovery_error;
        if (!p2::discover_thermal_device(&options.thermal, &discovery_error)) {
            std::cerr << discovery_error << '\n';
            return EXIT_FAILURE;
        }
    }

    std::vector<std::uint8_t> eeprom;
    if (!read_file(options.eeprom, &eeprom)) {
        std::cerr << "failed to read EEPROM " << options.eeprom << '\n';
        return EXIT_FAILURE;
    }
    p2::Mlx90640Math math;
    std::string error;
    if (!math.initialize_eeprom_be(eeprom.data(), eeprom.size(), &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    p2::ThermalMathConfig math_config;
    math_config.emissivity = options.emissivity;
    math_config.reflected_temperature_offset_c =
        options.reflected_offset_c;
    p2::ThermalCaptureConfig capture_config;
    capture_config.device = options.thermal;
    p2::ThermalCapture capture(capture_config);
    std::atomic<bool> stop{false};
    RealtimeStats stats;
    const auto started = std::chrono::steady_clock::now();
    const bool capture_result = capture.run(
        stop,
        [&](const p2::ThermalFramePayload &frame) {
            p2::ThermalMathResult result;
            if (!math.calculate(frame, math_config, &result, &error)) {
                ++stats.math_failures;
                stop.store(true);
                return;
            }
            update_stats(result, &stats);
            if (stats.pairs >= options.requested_pairs)
                stop.store(true);
        },
        &error);
    const std::uint64_t elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    const p2::ThermalCaptureStats capture_stats = capture.stats();
    const bool passed = capture_result && error.empty() &&
        stats.pairs == options.requested_pairs && stats.math_failures == 0 &&
        capture_stats.invalid_payloads == 0 &&
        capture_stats.sequence_gaps == 0 &&
        capture_stats.timestamp_mismatches == 0;

    write_report(std::cout, options, stats, capture_stats, elapsed_ms,
                 passed, error);
    std::ofstream output(options.output);
    if (!output) {
        std::cerr << "failed to open report " << options.output << '\n';
        return EXIT_FAILURE;
    }
    write_report(output, options, stats, capture_stats, elapsed_ms,
                 passed, error);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
