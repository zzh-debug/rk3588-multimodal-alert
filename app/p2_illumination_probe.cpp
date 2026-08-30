#include "p2/capture/device_discovery.hpp"
#include "p2/capture/v4l2_utils.hpp"
#include "p2/capture/visible_capture.hpp"
#include "p2/illumination/illumination_controller.hpp"
#include "p2/illumination/illumination_hardware.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

std::atomic<bool> g_stop{false};

struct Options {
    std::string visible;
    std::string sensor_subdevice;
    std::string led_path = "/sys/class/leds/zzh:white:fill";
    std::string summary = "/tmp/p2-illumination-summary.json";
    std::string events = "/tmp/p2-illumination-events.csv";
    std::uint64_t duration_seconds = 15;
    std::uint64_t forced_thermal_seconds = 0;
    bool force_dark = false;
    bool write_led = false;
    std::uint32_t target_brightness = 32;
};

void signal_handler(int)
{
    g_stop.store(true);
}

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --visible PATH              override RKISP mainpath\n"
        << "  --sensor-subdev PATH        override IMX415 subdevice\n"
        << "  --led-path PATH             override PWM LED class path\n"
        << "  --duration-sec N            run duration, default 15\n"
        << "  --target-brightness N       safety-capped target, default 32\n"
        << "  --force-dark                inject dark telemetry for gate only\n"
        << "  --forced-thermal-sec N      inject target for first N seconds\n"
        << "  --write-led                 apply controller output to PWM LED\n"
        << "  --summary FILE              JSON output\n"
        << "  --events FILE               per-frame CSV output\n"
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

bool parse_options(int argc, char **argv, Options *options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (argument == "--force-dark") {
            options->force_dark = true;
            continue;
        }
        if (argument == "--write-led") {
            options->write_led = true;
            continue;
        }
        if (index + 1 >= argc)
            return false;
        const char *value = argv[++index];
        if (argument == "--visible")
            options->visible = value;
        else if (argument == "--sensor-subdev")
            options->sensor_subdevice = value;
        else if (argument == "--led-path")
            options->led_path = value;
        else if (argument == "--summary")
            options->summary = value;
        else if (argument == "--events")
            options->events = value;
        else if (argument == "--duration-sec") {
            if (!parse_u64(value, &options->duration_seconds))
                return false;
        } else if (argument == "--forced-thermal-sec") {
            if (!parse_u64(value, &options->forced_thermal_seconds))
                return false;
        } else if (argument == "--target-brightness") {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, &parsed) || parsed > 32U)
                return false;
            options->target_brightness =
                static_cast<std::uint32_t>(parsed);
        } else {
            return false;
        }
    }
    return options->duration_seconds > 0U &&
        options->duration_seconds <= 3600U &&
        options->target_brightness > 0U &&
        options->forced_thermal_seconds <= options->duration_seconds &&
        !options->summary.empty() && !options->events.empty();
}

void write_summary(std::ostream &output, const Options &options,
                   const p2::VisibleCaptureStats &capture,
                   const p2::IlluminationStats &controller,
                   const p2::PwmLedStats &led,
                   std::uint64_t frames,
                   std::uint64_t dark_frames,
                   double mean_p50,
                   double mean_p90,
                   double mean_exposure_ratio,
                   double mean_gain_ratio,
                   double elapsed_seconds,
                   bool passed,
                   const std::string &error)
{
    const double denominator = frames == 0U
        ? 1.0 : static_cast<double>(frames);
    output << std::fixed << std::setprecision(3)
           << "{\n"
           << "  \"schema\": \"p2.illumination-probe.v1\",\n"
           << "  \"result\": \"" << (passed ? "PASS" : "FAIL")
           << "\",\n"
           << "  \"error\": \"" << error << "\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT
           << "\",\n"
           << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n"
           << "  \"mode\": {\"force_dark\": "
           << (options.force_dark ? "true" : "false")
           << ", \"forced_thermal_seconds\": "
           << options.forced_thermal_seconds
           << ", \"write_led\": "
           << (options.write_led ? "true" : "false") << "},\n"
           << "  \"capture\": {\"frames\": " << frames
           << ", \"dqbuf\": " << capture.dqbuf_count
           << ", \"sequence_gaps\": " << capture.sequence_gaps
           << ", \"bad_bytes_used\": " << capture.bad_bytes_used
           << ", \"missing_timestamp\": "
           << capture.missing_monotonic_timestamp << "},\n"
           << "  \"telemetry\": {\"dark_frames\": " << dark_frames
           << ", \"mean_p50\": " << mean_p50 / denominator
           << ", \"mean_p90\": " << mean_p90 / denominator
           << ", \"mean_exposure_ratio\": "
           << mean_exposure_ratio / denominator
           << ", \"mean_gain_ratio\": "
           << mean_gain_ratio / denominator << "},\n"
           << "  \"controller\": {\"updates\": "
           << controller.updates << ", \"transitions\": "
           << controller.transitions << ", \"activations\": "
           << controller.activations << ", \"deactivations\": "
           << controller.deactivations << ", \"faults\": "
           << controller.faults << ", \"maximum_brightness\": "
           << controller.maximum_brightness << "},\n"
           << "  \"led\": {\"writes\": " << led.writes
           << ", \"failures\": " << led.failures
           << ", \"maximum_commanded\": " << led.maximum_commanded
           << ", \"final_brightness\": " << led.final_brightness
           << "}\n"
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
    p2::SysfsPwmLed led(options.led_path);
    if (options.write_led &&
        !led.initialize(options.target_brightness, &error)) {
        std::cerr << "LED initialization failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    p2::Imx415ExposureMonitor exposure_monitor;
    if (!exposure_monitor.initialize(options.sensor_subdevice, &error)) {
        std::cerr << "exposure monitor initialization failed: "
                  << error << '\n';
        return EXIT_FAILURE;
    }
    p2::IlluminationConfig config;
    config.target_brightness = options.target_brightness;
    config.ramp_step = std::min<std::uint32_t>(
        4U, options.target_brightness);
    p2::IlluminationController controller(config);
    std::ofstream events(options.events);
    if (!events) {
        std::cerr << "cannot open illumination event CSV\n";
        return EXIT_FAILURE;
    }
    events << "timestamp_ns,sequence,observed_p50,observed_p90,"
              "effective_p50,effective_p90,exposure,gain,exposure_ratio,"
              "gain_ratio,thermal_target,dark_condition,state,ramp,"
              "brightness,state_changed\n";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    p2::VisibleCapture capture({
        options.visible, 3840, 2160, 6, 500, false,
    });
    const std::uint64_t started_ns = p2::monotonic_now_ns();
    const std::uint64_t deadline_ns = started_ns +
        options.duration_seconds * 1'000'000'000ULL;
    const std::uint64_t forced_target_until_ns = started_ns +
        options.forced_thermal_seconds * 1'000'000'000ULL;
    std::uint64_t frames = 0;
    std::uint64_t dark_frames = 0;
    double p50_sum = 0.0;
    double p90_sum = 0.0;
    double exposure_ratio_sum = 0.0;
    double gain_ratio_sum = 0.0;
    bool processing_ok = true;
    const bool capture_ok = capture.run_frames(
        g_stop,
        [&](const p2::VisibleFrameView &frame) {
            if (p2::monotonic_now_ns() >= deadline_ns) {
                g_stop.store(true);
                return;
            }
            p2::LumaStatistics observed;
            p2::ExposureTelemetry exposure;
            std::string frame_error;
            if (!p2::analyze_nv12_luma(
                    frame.data + frame.data_offset,
                    frame.size - frame.data_offset,
                    frame.width, frame.height, frame.bytes_per_line,
                    &observed, &frame_error) ||
                !exposure_monitor.sample(&exposure, &frame_error)) {
                processing_ok = false;
                error = frame_error;
                g_stop.store(true);
                return;
            }
            p2::LumaStatistics effective = observed;
            p2::ExposureTelemetry effective_exposure = exposure;
            if (options.force_dark) {
                effective.mean = 20.0F;
                effective.p10 = 10;
                effective.p50 = 20;
                effective.p90 = 30;
                effective.dark_fraction = 0.8F;
                effective.bright_fraction = 0.0F;
                effective_exposure.exposure_ratio = 1.0F;
                effective_exposure.analogue_gain_ratio = 1.0F;
            }
            p2::IlluminationObservation input;
            input.timestamp_ns = frame.event.timestamp_ns;
            input.luma = effective;
            input.exposure = effective_exposure;
            input.sustained_thermal_target =
                options.forced_thermal_seconds != 0U &&
                frame.event.timestamp_ns < forced_target_until_ns;
            p2::IlluminationUpdate update;
            if (!controller.update(input, &update, &frame_error)) {
                processing_ok = false;
                error = frame_error;
                if (options.write_led)
                    led.off(nullptr);
                g_stop.store(true);
                return;
            }
            if (options.write_led &&
                !led.set_brightness(update.desired_brightness,
                                    &frame_error)) {
                controller.force_fault(&update);
                led.off(nullptr);
                processing_ok = false;
                error = frame_error;
                g_stop.store(true);
                return;
            }
            ++frames;
            dark_frames += update.dark_condition ? 1U : 0U;
            p50_sum += observed.p50;
            p90_sum += observed.p90;
            exposure_ratio_sum += exposure.exposure_ratio;
            gain_ratio_sum += exposure.analogue_gain_ratio;
            events << frame.event.timestamp_ns << ','
                   << frame.event.sequence << ','
                   << static_cast<unsigned>(observed.p50) << ','
                   << static_cast<unsigned>(observed.p90) << ','
                   << static_cast<unsigned>(effective.p50) << ','
                   << static_cast<unsigned>(effective.p90) << ','
                   << exposure.exposure << ',' << exposure.analogue_gain
                   << ',' << exposure.exposure_ratio << ','
                   << exposure.analogue_gain_ratio << ','
                   << (input.sustained_thermal_target ? 1 : 0) << ','
                   << (update.dark_condition ? 1 : 0) << ','
                   << p2::to_string(update.state) << ','
                   << p2::to_string(update.ramp_direction) << ','
                   << update.desired_brightness << ','
                   << (update.state_changed ? 1 : 0) << '\n';
        },
        &error);
    if (options.write_led && !led.off(&error))
        processing_ok = false;
    const double elapsed_seconds = static_cast<double>(
        p2::monotonic_now_ns() - started_ns) / 1.0e9;
    const p2::VisibleCaptureStats capture_stats = capture.stats();
    const p2::IlluminationStats controller_stats = controller.stats();
    const p2::PwmLedStats led_stats = led.stats();
    const bool forced_cycle = options.force_dark &&
        options.forced_thermal_seconds != 0U;
    const bool passed = capture_ok && processing_ok && frames > 0U &&
        capture_stats.sequence_gaps == 0U &&
        capture_stats.bad_bytes_used == 0U &&
        capture_stats.missing_monotonic_timestamp == 0U &&
        controller_stats.faults == 0U &&
        (!options.write_led ||
         (led_stats.failures == 0U && led_stats.final_brightness == 0U)) &&
        (!forced_cycle ||
         (controller_stats.activations > 0U &&
          controller_stats.deactivations > 0U &&
          controller_stats.maximum_brightness ==
              options.target_brightness));
    write_summary(std::cout, options, capture_stats, controller_stats,
                  led_stats, frames, dark_frames, p50_sum, p90_sum,
                  exposure_ratio_sum, gain_ratio_sum, elapsed_seconds,
                  passed, error);
    std::ofstream summary(options.summary);
    if (!summary) {
        std::cerr << "cannot open illumination summary JSON\n";
        return EXIT_FAILURE;
    }
    write_summary(summary, options, capture_stats, controller_stats,
                  led_stats, frames, dark_frames, p50_sum, p90_sum,
                  exposure_ratio_sum, gain_ratio_sum, elapsed_seconds,
                  passed, error);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
