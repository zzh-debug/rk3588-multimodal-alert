#include "p2/capture/device_discovery.hpp"
#include "p2/capture/thermal_capture.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace {

struct Options {
    std::string thermal;
    std::string output;
};

void print_usage(const char *program)
{
    std::cout << "Usage: " << program
              << " --output FILE [--thermal PATH]\n";
}

bool parse_options(int argc, char **argv, Options *options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (index + 1 >= argc)
            return false;
        const char *value = argv[++index];
        if (argument == "--thermal")
            options->thermal = value;
        else if (argument == "--output")
            options->output = value;
        else
            return false;
    }
    return !options->output.empty();
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (options.thermal.empty()) {
        std::string discovery_error;
        if (!p2::discover_thermal_device(&options.thermal, &discovery_error)) {
            std::cerr << "thermal discovery failed: " << discovery_error
                      << '\n';
            return EXIT_FAILURE;
        }
    }

    p2::ThermalCaptureConfig config;
    config.device = options.thermal;
    p2::ThermalCapture capture(config);
    std::atomic<bool> stop{false};
    bool saved = false;
    std::string capture_error;
    const bool capture_result = capture.run(
        stop,
        [&](const p2::ThermalFramePayload &frame) {
            std::ofstream output(options.output, std::ios::binary);
            if (!output) {
                capture_error = "open output " + options.output + ": " +
                    std::strerror(errno);
                stop.store(true);
                return;
            }
            output.write(
                reinterpret_cast<const char *>(frame.zmlx_bytes.data()),
                static_cast<std::streamsize>(frame.zmlx_bytes.size()));
            if (!output.good()) {
                capture_error = "write output " + options.output;
                stop.store(true);
                return;
            }
            std::cout << "THERMAL_DEVICE=" << options.thermal << '\n'
                      << "PAIR_SEQUENCE=" << frame.event.pair_sequence << '\n'
                      << "EVENT_TIMESTAMP_NS=" << frame.event.timestamp_ns
                      << '\n'
                      << "SUBPAGE_SPAN_NS="
                      << frame.event.second_ready_ns -
                             frame.event.first_ready_ns
                      << '\n'
                      << "ZMLX_BYTES=" << frame.zmlx_bytes.size() << '\n'
                      << "OUTPUT=" << options.output << '\n';
            saved = true;
            stop.store(true);
        },
        &capture_error);

    if (!capture_result || !saved || !capture_error.empty()) {
        std::cerr << "thermal export failed: " << capture_error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "P2_THERMAL_EXPORT=PASS\n";
    return EXIT_SUCCESS;
}
