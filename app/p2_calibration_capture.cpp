#include "p2/capture/device_discovery.hpp"
#include "p2/capture/thermal_capture.hpp"
#include "p2/capture/visible_capture.hpp"
#include "p2/thermal/colormap.hpp"
#include "p2/thermal/mlx90640_math.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

constexpr const char *kProjectOneCommit =
    "c140532b82723bdfe1396b51304f2dfe9c4c73e7";

struct Options {
    std::string visible;
    std::string thermal;
    std::string eeprom =
        "/sys/bus/nvmem/devices/zzh_mlx90640_eeprom-5-33/nvmem";
    std::string output = "/tmp/p2-calibration-capture";
    std::uint64_t samples = 1;
    std::uint64_t interval_ms = 500;
    std::uint64_t max_skew_ms = 100;
    std::uint64_t timeout_seconds = 30;
    float emissivity = 0.95F;
    float reflected_offset_c = -8.0F;
    float color_min_c = 20.0F;
    float color_max_c = 45.0F;
};

struct CalibrationSample {
    p2::VisibleFrameEvent visible;
    std::uint32_t visible_width = 0;
    std::uint32_t visible_height = 0;
    std::uint32_t visible_source_stride = 0;
    std::vector<std::uint8_t> visible_y;
    p2::ThermalFramePayload thermal;
    p2::ThermalMathResult thermal_math;
    p2::ThermalColorMapResult thermal_colors;
    std::int64_t signed_skew_ns = 0;
};

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --visible PATH          override discovered RKISP mainpath\n"
        << "  --thermal PATH          override discovered ZMLX node\n"
        << "  --eeprom FILE           MLX90640 NVMEM path\n"
        << "  --output DIR            output directory\n"
        << "  --samples N             number of sample pairs, default 1\n"
        << "  --interval-ms N         minimum thermal interval, default 500\n"
        << "  --max-skew-ms N         maximum following-frame skew, default 100\n"
        << "  --timeout-sec N         whole capture timeout, default 30\n"
        << "  --emissivity VALUE      default 0.95\n"
        << "  --reflected-offset-c V  default -8\n"
        << "  --color-min-c VALUE     colormap lower bound, default 20\n"
        << "  --color-max-c VALUE     colormap upper bound, default 45\n"
        << "  --help                  show this text\n";
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
        else if (argument == "--output")
            options->output = value;
        else if (argument == "--samples") {
            if (!parse_u64(value, &options->samples) || options->samples == 0)
                return false;
        } else if (argument == "--interval-ms") {
            if (!parse_u64(value, &options->interval_ms))
                return false;
        } else if (argument == "--max-skew-ms") {
            if (!parse_u64(value, &options->max_skew_ms) ||
                options->max_skew_ms == 0)
                return false;
        } else if (argument == "--timeout-sec") {
            if (!parse_u64(value, &options->timeout_seconds) ||
                options->timeout_seconds == 0)
                return false;
        } else if (argument == "--emissivity") {
            if (!parse_float(value, &options->emissivity))
                return false;
        } else if (argument == "--reflected-offset-c") {
            if (!parse_float(value, &options->reflected_offset_c))
                return false;
        } else if (argument == "--color-min-c") {
            if (!parse_float(value, &options->color_min_c))
                return false;
        } else if (argument == "--color-max-c") {
            if (!parse_float(value, &options->color_max_c))
                return false;
        } else {
            return false;
        }
    }
    return !options->eeprom.empty() && !options->output.empty() &&
        options->samples <= 100 &&
        options->interval_ms <=
            std::numeric_limits<std::uint64_t>::max() / 1'000'000ULL &&
        options->max_skew_ms <=
            std::numeric_limits<std::uint64_t>::max() / 1'000'000ULL &&
        std::isfinite(options->emissivity) && options->emissivity > 0.0F &&
        options->emissivity <= 1.0F &&
        std::isfinite(options->reflected_offset_c) &&
        std::isfinite(options->color_min_c) &&
        std::isfinite(options->color_max_c) &&
        options->color_max_c > options->color_min_c;
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

bool make_directories(const std::string &path)
{
    if (path.empty())
        return false;
    std::string current = path.front() == '/' ? "/" : "";
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

class CaptureCoordinator {
public:
    CaptureCoordinator(std::uint64_t requested_samples,
                       std::uint64_t interval_ns,
                       std::uint64_t max_skew_ns,
                       std::atomic<bool> *stop)
        : requested_samples_(requested_samples), interval_ns_(interval_ns),
          max_skew_ns_(max_skew_ns), stop_(stop)
    {
    }

    void submit_thermal(const p2::ThermalFramePayload &frame,
                        const p2::ThermalMathResult &math,
                        const p2::ThermalColorMapResult &colors)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error_.empty() || pending_ ||
            samples_.size() >= requested_samples_ ||
            frame.event.timestamp_ns < next_thermal_timestamp_ns_)
            return;
        pending_sample_.thermal = frame;
        pending_sample_.thermal_math = math;
        pending_sample_.thermal_colors = colors;
        pending_ = true;
    }

    void submit_visible(const p2::VisibleFrameView &frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_ || !error_.empty())
            return;
        const std::uint64_t thermal_timestamp =
            pending_sample_.thermal.event.timestamp_ns;
        if (frame.event.timestamp_ns < thermal_timestamp)
            return;
        const std::uint64_t skew = frame.event.timestamp_ns - thermal_timestamp;
        if (skew > max_skew_ns_) {
            pending_ = false;
            ++skew_rejections_;
            return;
        }
        if (frame.data == nullptr || frame.width == 0 || frame.height == 0 ||
            frame.bytes_per_line < frame.width ||
            static_cast<std::uint64_t>(frame.bytes_per_line) *
                    frame.height > frame.size) {
            fail_locked("visible frame payload/stride is invalid");
            return;
        }

        pending_sample_.visible = frame.event;
        pending_sample_.visible_width = frame.width;
        pending_sample_.visible_height = frame.height;
        pending_sample_.visible_source_stride = frame.bytes_per_line;
        pending_sample_.visible_y.resize(
            static_cast<std::size_t>(frame.width) * frame.height);
        for (std::uint32_t row = 0; row < frame.height; ++row) {
            std::memcpy(
                pending_sample_.visible_y.data() +
                    static_cast<std::size_t>(row) * frame.width,
                frame.data + static_cast<std::size_t>(row) *
                    frame.bytes_per_line,
                frame.width);
        }
        pending_sample_.signed_skew_ns = static_cast<std::int64_t>(skew);
        next_thermal_timestamp_ns_ = thermal_timestamp + interval_ns_;
        samples_.push_back(std::move(pending_sample_));
        pending_sample_ = CalibrationSample{};
        pending_ = false;
        if (samples_.size() >= requested_samples_)
            stop_->store(true);
        condition_.notify_all();
    }

    void fail(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_locked(message);
    }

    bool wait(std::chrono::seconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool completed = condition_.wait_for(lock, timeout, [&]() {
            return !error_.empty() || samples_.size() >= requested_samples_;
        });
        if (!completed)
            fail_locked("calibration capture timed out");
        return error_.empty() && samples_.size() >= requested_samples_;
    }

    std::vector<CalibrationSample> take_samples()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::move(samples_);
    }

    std::string error() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    std::uint64_t skew_rejections() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return skew_rejections_;
    }

private:
    void fail_locked(const std::string &message)
    {
        if (error_.empty())
            error_ = message;
        stop_->store(true);
        condition_.notify_all();
    }

    const std::uint64_t requested_samples_;
    const std::uint64_t interval_ns_;
    const std::uint64_t max_skew_ns_;
    std::atomic<bool> *stop_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<CalibrationSample> samples_;
    CalibrationSample pending_sample_;
    bool pending_ = false;
    std::uint64_t next_thermal_timestamp_ns_ = 0;
    std::uint64_t skew_rejections_ = 0;
    std::string error_;
};

bool write_visible_pgm(const std::string &path,
                       const CalibrationSample &sample)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        return false;
    output << "P5\n" << sample.visible_width << ' '
           << sample.visible_height << "\n255\n";
    output.write(reinterpret_cast<const char *>(sample.visible_y.data()),
                 static_cast<std::streamsize>(sample.visible_y.size()));
    return output.good();
}

bool write_temperature_csv(const std::string &path,
                           const CalibrationSample &sample)
{
    std::ofstream output(path);
    if (!output)
        return false;
    output << "row";
    for (std::size_t column = 0; column < p2::kMlx90640Width; ++column)
        output << ",c" << column;
    output << '\n' << std::fixed << std::setprecision(6);
    for (std::size_t row = 0; row < p2::kMlx90640Height; ++row) {
        output << row;
        for (std::size_t column = 0; column < p2::kMlx90640Width; ++column) {
            output << ',' << sample.thermal_math.temperature_c[
                row * p2::kMlx90640Width + column];
        }
        output << '\n';
    }
    return output.good();
}

bool write_thermal_ppm(const std::string &path,
                       const CalibrationSample &sample)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        return false;
    output << "P6\n" << p2::kMlx90640Width << ' '
           << p2::kMlx90640Height << "\n255\n";
    for (const p2::Rgb8 &pixel : sample.thermal_colors.rgb) {
        const std::array<char, 3> bytes{
            static_cast<char>(pixel.red), static_cast<char>(pixel.green),
            static_cast<char>(pixel.blue)};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    return output.good();
}

bool write_zmlx(const std::string &path, const CalibrationSample &sample)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
        return false;
    output.write(
        reinterpret_cast<const char *>(sample.thermal.zmlx_bytes.data()),
        static_cast<std::streamsize>(sample.thermal.zmlx_bytes.size()));
    return output.good();
}

bool write_sample_manifest(const std::string &path,
                           const CalibrationSample &sample,
                           const Options &options,
                           const p2::DiscoveredDevices &devices)
{
    std::ofstream output(path);
    if (!output)
        return false;
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"schema\": \"p2.calibration-sample.v1\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT << "\",\n"
           << "  \"project_one_commit\": \"" << kProjectOneCommit << "\",\n"
           << "  \"thermal_model\": \"MLX90640-ESF-BAA\",\n"
           << "  \"expected_fov_degrees\": [110.0, 75.0],\n"
           << "  \"visible_device\": \"" << devices.visible << "\",\n"
           << "  \"thermal_device\": \"" << devices.thermal << "\",\n"
           << "  \"visible_sequence\": " << sample.visible.sequence << ",\n"
           << "  \"visible_timestamp_ns\": "
           << sample.visible.timestamp_ns << ",\n"
           << "  \"visible_width\": " << sample.visible_width << ",\n"
           << "  \"visible_height\": " << sample.visible_height << ",\n"
           << "  \"visible_source_stride\": "
           << sample.visible_source_stride << ",\n"
           << "  \"thermal_sequence\": " << sample.thermal.event.sequence
           << ",\n"
           << "  \"thermal_pair_sequence\": "
           << sample.thermal.event.pair_sequence << ",\n"
           << "  \"thermal_timestamp_ns\": "
           << sample.thermal.event.timestamp_ns << ",\n"
           << "  \"signed_visible_minus_thermal_ns\": "
           << sample.signed_skew_ns << ",\n"
           << "  \"capture_strategy\": \"first visible frame dequeued after a complete thermal pair\",\n"
           << "  \"thermal_subpage_span_ns\": "
           << sample.thermal_math.subpage_span_ns << ",\n"
           << "  \"maximum_allowed_skew_ms\": "
           << options.max_skew_ms << ",\n"
           << "  \"emissivity\": " << options.emissivity << ",\n"
           << "  \"reflected_temperature_offset_c\": "
           << options.reflected_offset_c << ",\n"
           << "  \"ambient_temperature_c\": "
           << sample.thermal_math.ta_c << ",\n"
           << "  \"scene_min_c\": "
           << sample.thermal_math.min_temperature_c << ",\n"
           << "  \"scene_max_c\": "
           << sample.thermal_math.max_temperature_c << ",\n"
           << "  \"files\": {\n"
           << "    \"visible_luma\": \"visible-y.pgm\",\n"
           << "    \"thermal_temperature\": \"thermal-temperature.csv\",\n"
           << "    \"thermal_colormap\": \"thermal-colormap.ppm\",\n"
           << "    \"thermal_zmlx\": \"thermal-zmlx.bin\"\n"
           << "  },\n"
           << "  \"scope\": \"static planar target capture; not a calibrated mapping\"\n"
           << "}\n";
    return output.good();
}

bool write_outputs(const std::string &root,
                   const std::vector<CalibrationSample> &samples,
                   const Options &options,
                   const p2::DiscoveredDevices &devices,
                   std::uint64_t skew_rejections)
{
    if (!make_directories(root))
        return false;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        std::ostringstream name;
        name << root << "/sample-" << std::setfill('0') << std::setw(3)
             << index;
        const std::string directory = name.str();
        if (!make_directories(directory) ||
            !write_visible_pgm(directory + "/visible-y.pgm", samples[index]) ||
            !write_temperature_csv(directory + "/thermal-temperature.csv",
                                   samples[index]) ||
            !write_thermal_ppm(directory + "/thermal-colormap.ppm",
                               samples[index]) ||
            !write_zmlx(directory + "/thermal-zmlx.bin", samples[index]) ||
            !write_sample_manifest(directory + "/manifest.json", samples[index],
                                   options, devices))
            return false;
    }
    std::ofstream summary(root + "/capture-summary.json");
    if (!summary)
        return false;
    summary << "{\n"
            << "  \"schema\": \"p2.calibration-capture.v1\",\n"
            << "  \"result\": \"PASS\",\n"
            << "  \"application_commit\": \"" << P2_GIT_COMMIT << "\",\n"
            << "  \"thermal_model\": \"MLX90640-ESF-BAA\",\n"
            << "  \"expected_fov_degrees\": [110.0, 75.0],\n"
            << "  \"captured_samples\": " << samples.size() << ",\n"
            << "  \"skew_rejections\": " << skew_rejections << "\n"
            << "}\n";
    return summary.good();
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
    if (options.visible.empty() && options.thermal.empty()) {
        if (!p2::discover_devices(&devices, &error)) {
            std::cerr << error << '\n';
            return EXIT_FAILURE;
        }
    } else {
        if (options.visible.empty()) {
            if (!p2::discover_visible_device(
                    &devices.visible, &devices.visible_media, &error)) {
                std::cerr << error << '\n';
                return EXIT_FAILURE;
            }
        } else {
            devices.visible = options.visible;
        }
        if (options.thermal.empty()) {
            if (!p2::discover_thermal_device(&devices.thermal, &error)) {
                std::cerr << error << '\n';
                return EXIT_FAILURE;
            }
        } else {
            devices.thermal = options.thermal;
        }
    }
    options.visible = devices.visible;
    options.thermal = devices.thermal;

    std::vector<std::uint8_t> eeprom;
    if (!read_file(options.eeprom, &eeprom)) {
        std::cerr << "failed to read EEPROM " << options.eeprom << '\n';
        return EXIT_FAILURE;
    }
    p2::Mlx90640Math math;
    if (!math.initialize_eeprom_be(eeprom.data(), eeprom.size(), &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    p2::ThermalMathConfig math_config;
    math_config.emissivity = options.emissivity;
    math_config.reflected_temperature_offset_c = options.reflected_offset_c;
    p2::ThermalColorMapConfig color_config;
    color_config.min_temperature_c = options.color_min_c;
    color_config.max_temperature_c = options.color_max_c;

    std::atomic<bool> stop{false};
    CaptureCoordinator coordinator(
        options.samples, options.interval_ms * 1'000'000ULL,
        options.max_skew_ms * 1'000'000ULL, &stop);
    p2::VisibleCapture visible({devices.visible, 3840, 2160, 6, 500});
    p2::ThermalCapture thermal({devices.thermal, 4, 1000});
    bool visible_ok = false;
    bool thermal_ok = false;
    std::string visible_error;
    std::string thermal_error;

    std::thread visible_thread([&]() {
        visible_ok = visible.run_frames(
            stop,
            [&](const p2::VisibleFrameView &frame) {
                coordinator.submit_visible(frame);
            },
            &visible_error);
        if (!visible_ok && !stop.load())
            coordinator.fail("visible capture: " + visible_error);
    });
    std::thread thermal_thread([&]() {
        thermal_ok = thermal.run(
            stop,
            [&](const p2::ThermalFramePayload &frame) {
                p2::ThermalMathResult result;
                std::string callback_error;
                if (!math.calculate(frame, math_config, &result,
                                    &callback_error)) {
                    coordinator.fail("thermal math: " + callback_error);
                    return;
                }
                if (result.finite_temperature_pixels != p2::kMlx90640Pixels) {
                    coordinator.fail(
                        "thermal math did not produce 768 finite pixels");
                    return;
                }
                p2::ThermalColorMapResult colors;
                if (!p2::apply_thermal_colormap(
                        result.temperature_c, color_config, &colors,
                        &callback_error)) {
                    coordinator.fail("thermal colormap: " + callback_error);
                    return;
                }
                if (colors.invalid_pixels != 0) {
                    coordinator.fail("thermal colormap contains invalid pixels");
                    return;
                }
                coordinator.submit_thermal(frame, result, colors);
            },
            &thermal_error);
        if (!thermal_ok && !stop.load())
            coordinator.fail("thermal capture: " + thermal_error);
    });

    const bool completed = coordinator.wait(
        std::chrono::seconds(options.timeout_seconds));
    stop.store(true);
    visible_thread.join();
    thermal_thread.join();

    std::vector<CalibrationSample> samples = coordinator.take_samples();
    const std::string coordinator_error = coordinator.error();
    if (!completed || !visible_ok || !thermal_ok || !coordinator_error.empty()) {
        std::cerr << "calibration capture failed: "
                  << (coordinator_error.empty() ? "capture thread failure"
                                                : coordinator_error)
                  << '\n';
        if (!visible_error.empty())
            std::cerr << "visible: " << visible_error << '\n';
        if (!thermal_error.empty())
            std::cerr << "thermal: " << thermal_error << '\n';
        return EXIT_FAILURE;
    }
    if (!write_outputs(options.output, samples, options, devices,
                       coordinator.skew_rejections())) {
        std::cerr << "failed to write calibration capture under "
                  << options.output << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "captured " << samples.size() << " calibration sample(s) to "
              << options.output << " using MLX90640-ESF-BAA (110x75 deg)\n";
    return EXIT_SUCCESS;
}
