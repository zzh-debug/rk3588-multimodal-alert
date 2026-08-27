#include "p2/thermal/mlx90640_math.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string eeprom;
    std::string zmlx;
    std::string output;
    float emissivity = 0.95F;
    float reflected_offset_c = -8.0F;
    bool correct_bad_pixels = true;
};

void usage(const char *program)
{
    std::cout << "Usage: " << program
              << " --eeprom FILE --zmlx FILE [--output FILE]"
                 " [--emissivity VALUE] [--reflected-offset-c VALUE]"
                 " [--no-bad-pixel-correction]\n";
}

bool parse_float(const char *text, float *value)
{
    char *end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0')
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
        if (argument == "--no-bad-pixel-correction") {
            options->correct_bad_pixels = false;
            continue;
        }
        if (index + 1 >= argc)
            return false;
        const char *value = argv[++index];
        if (argument == "--eeprom")
            options->eeprom = value;
        else if (argument == "--zmlx")
            options->zmlx = value;
        else if (argument == "--output")
            options->output = value;
        else if (argument == "--emissivity") {
            if (!parse_float(value, &options->emissivity))
                return false;
        } else if (argument == "--reflected-offset-c") {
            if (!parse_float(value, &options->reflected_offset_c))
                return false;
        } else {
            return false;
        }
    }
    return !options->eeprom.empty() && !options->zmlx.empty();
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

void write_result(std::ostream &output, const Options &options,
                  const p2::ThermalMathResult &result)
{
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"schema\": \"p2.thermal-math.v1\",\n"
           << "  \"result\": \"PASS\",\n"
           << "  \"emissivity\": " << options.emissivity << ",\n"
           << "  \"reflected_temperature_offset_c\": "
           << options.reflected_offset_c << ",\n"
           << "  \"bad_pixel_correction\": "
           << (options.correct_bad_pixels ? "true" : "false") << ",\n"
           << "  \"pair_sequence\": " << result.pair_sequence << ",\n"
           << "  \"subpage_ids\": [" << result.subpage_id[0] << ", "
           << result.subpage_id[1] << "],\n"
           << "  \"subpage_span_ns\": " << result.subpage_span_ns
           << ",\n"
           << "  \"ta_c\": " << result.ta_c << ",\n"
           << "  \"vdd\": " << result.vdd << ",\n"
           << "  \"min_temperature_c\": "
           << result.min_temperature_c << ",\n"
           << "  \"max_temperature_c\": "
           << result.max_temperature_c << ",\n"
           << "  \"finite_temperature_pixels\": "
           << result.finite_temperature_pixels << ",\n"
           << "  \"finite_image_pixels\": "
           << result.finite_image_pixels << ",\n"
           << "  \"broken_pixel_count\": "
           << result.broken_pixel_count << ",\n"
           << "  \"outlier_pixel_count\": "
           << result.outlier_pixel_count << ",\n"
           << "  \"broken_pixels\": [";
    for (std::size_t index = 0; index < result.broken_pixel_count; ++index) {
        if (index != 0)
            output << ", ";
        output << result.broken_pixels[index];
    }
    output << "],\n"
           << "  \"outlier_pixels\": [";
    for (std::size_t index = 0; index < result.outlier_pixel_count; ++index) {
        if (index != 0)
            output << ", ";
        output << result.outlier_pixels[index];
    }
    output << "],\n"
           << "  \"calculation_time_ns\": "
           << result.calculation_time_ns << "\n"
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

    std::vector<std::uint8_t> eeprom;
    std::vector<std::uint8_t> zmlx;
    if (!read_file(options.eeprom, &eeprom) ||
        !read_file(options.zmlx, &zmlx)) {
        std::cerr << "failed to read input files\n";
        return EXIT_FAILURE;
    }
    if (zmlx.size() != p2::kZmlxMetaV1Bytes) {
        std::cerr << "ZMLX input must be exactly 3400 bytes\n";
        return EXIT_FAILURE;
    }

    p2::Mlx90640Math math;
    std::string error;
    if (!math.initialize_eeprom_be(eeprom.data(), eeprom.size(), &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    p2::ThermalFramePayload frame;
    std::copy(zmlx.begin(), zmlx.end(), frame.zmlx_bytes.begin());
    p2::ThermalMathConfig config;
    config.emissivity = options.emissivity;
    config.reflected_temperature_offset_c = options.reflected_offset_c;
    config.correct_bad_pixels = options.correct_bad_pixels;
    p2::ThermalMathResult result;
    if (!math.calculate(frame, config, &result, &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    write_result(std::cout, options, result);
    if (!options.output.empty()) {
        std::ofstream output(options.output);
        if (!output) {
            std::cerr << "failed to open " << options.output << '\n';
            return EXIT_FAILURE;
        }
        write_result(output, options, result);
    }
    return EXIT_SUCCESS;
}
