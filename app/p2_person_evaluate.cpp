#include "p2/evaluation/person_metrics.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

struct Options {
    std::string manifest;
    std::string annotations;
    std::string predictions;
    std::string json_output;
    std::string failures_output;
    float confidence_threshold = 0.25F;
    float iou_threshold = 0.50F;
    float minimum_precision = 0.0F;
    float minimum_recall = 0.0F;
};

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --manifest FILE      capture manifest.csv\n"
        << "  --annotations FILE   ground-truth annotations.csv\n"
        << "  --predictions FILE   detector predictions.csv\n"
        << "  --confidence VALUE   accepted confidence, default 0.25\n"
        << "  --iou VALUE          match IoU, default 0.50\n"
        << "  --min-precision V    optional pass gate, default 0\n"
        << "  --min-recall V       optional pass gate, default 0\n"
        << "  --json FILE          write evaluation JSON\n"
        << "  --failures FILE      write per-image FP/FN CSV\n"
        << "  --help               show this text\n";
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
        if (argument == "--manifest")
            options->manifest = value;
        else if (argument == "--annotations")
            options->annotations = value;
        else if (argument == "--predictions")
            options->predictions = value;
        else if (argument == "--json")
            options->json_output = value;
        else if (argument == "--failures")
            options->failures_output = value;
        else if (argument == "--confidence") {
            if (!parse_float(value, &options->confidence_threshold))
                return false;
        } else if (argument == "--iou") {
            if (!parse_float(value, &options->iou_threshold))
                return false;
        } else if (argument == "--min-precision") {
            if (!parse_float(value, &options->minimum_precision))
                return false;
        } else if (argument == "--min-recall") {
            if (!parse_float(value, &options->minimum_recall))
                return false;
        } else {
            return false;
        }
    }
    const auto in_unit_interval = [](float value) {
        return value >= 0.0F && value <= 1.0F;
    };
    return !options->manifest.empty() && !options->annotations.empty() &&
        !options->predictions.empty() &&
        in_unit_interval(options->confidence_threshold) &&
        options->iou_threshold > 0.0F && options->iou_threshold <= 1.0F &&
        in_unit_interval(options->minimum_precision) &&
        in_unit_interval(options->minimum_recall);
}

std::vector<std::string> split_csv(const std::string &line)
{
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const std::size_t comma = line.find(',', begin);
        if (comma == std::string::npos) {
            fields.push_back(line.substr(begin));
            break;
        }
        fields.push_back(line.substr(begin, comma - begin));
        begin = comma + 1U;
    }
    return fields;
}

bool parse_box(const std::vector<std::string> &fields,
               std::size_t offset,
               p2::FloatBox *box)
{
    return fields.size() >= offset + 4U &&
        parse_float(fields[offset].c_str(), &box->left) &&
        parse_float(fields[offset + 1U].c_str(), &box->top) &&
        parse_float(fields[offset + 2U].c_str(), &box->right) &&
        parse_float(fields[offset + 3U].c_str(), &box->bottom);
}

bool read_manifest(const std::string &path,
                   std::vector<std::string> *image_ids,
                   std::string *error)
{
    std::ifstream input(path);
    std::string line;
    if (!input || !std::getline(input, line) ||
        line != "image_id,image_file,sequence,timestamp_ns,source_width,"
                "source_height,preview_width,preview_height,rotation") {
        *error = "invalid or missing manifest header";
        return false;
    }
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty())
            continue;
        const std::vector<std::string> fields = split_csv(line);
        if (fields.size() != 9U || fields[0].empty()) {
            *error = "invalid manifest row at line " +
                std::to_string(line_number);
            return false;
        }
        image_ids->push_back(fields[0]);
    }
    return true;
}

bool read_annotations(const std::string &path,
                      std::vector<p2::PersonGroundTruth> *annotations,
                      std::string *error)
{
    std::ifstream input(path);
    std::string line;
    if (!input || !std::getline(input, line) ||
        line != "image_id,left,top,right,bottom") {
        *error = "invalid or missing annotations header";
        return false;
    }
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty())
            continue;
        const std::vector<std::string> fields = split_csv(line);
        p2::PersonGroundTruth truth;
        if (fields.size() != 5U || fields[0].empty() ||
            !parse_box(fields, 1U, &truth.box)) {
            *error = "invalid annotation row at line " +
                std::to_string(line_number);
            return false;
        }
        truth.image_id = fields[0];
        annotations->push_back(truth);
    }
    return true;
}

bool read_predictions(const std::string &path,
                      std::vector<p2::PersonPrediction> *predictions,
                      std::string *error)
{
    std::ifstream input(path);
    std::string line;
    if (!input || !std::getline(input, line) ||
        line != "image_id,confidence,left,top,right,bottom") {
        *error = "invalid or missing predictions header";
        return false;
    }
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty())
            continue;
        const std::vector<std::string> fields = split_csv(line);
        p2::PersonPrediction prediction;
        if (fields.size() != 6U || fields[0].empty() ||
            !parse_float(fields[1].c_str(), &prediction.confidence) ||
            !parse_box(fields, 2U, &prediction.box)) {
            *error = "invalid prediction row at line " +
                std::to_string(line_number);
            return false;
        }
        prediction.image_id = fields[0];
        predictions->push_back(prediction);
    }
    return true;
}

void write_json(std::ostream &output,
                const Options &options,
                const p2::PersonEvaluationResult &result,
                bool passed)
{
    output << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"schema\": \"p2.person-evaluation.v1\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT << "\",\n"
           << "  \"result\": \"" << (passed ? "PASS" : "FAIL") << "\",\n"
           << "  \"confidence_threshold\": "
           << options.confidence_threshold << ",\n"
           << "  \"iou_threshold\": " << options.iou_threshold << ",\n"
           << "  \"minimum_precision\": " << options.minimum_precision
           << ",\n"
           << "  \"minimum_recall\": " << options.minimum_recall << ",\n"
           << "  \"images\": " << result.images << ",\n"
           << "  \"positive_images\": " << result.positive_images << ",\n"
           << "  \"negative_images\": " << result.negative_images << ",\n"
           << "  \"ground_truth_instances\": "
           << result.ground_truth_instances << ",\n"
           << "  \"accepted_predictions\": "
           << result.accepted_predictions << ",\n"
           << "  \"true_positives\": " << result.true_positives << ",\n"
           << "  \"false_positives\": " << result.false_positives << ",\n"
           << "  \"false_negatives\": " << result.false_negatives << ",\n"
           << "  \"precision\": " << result.precision << ",\n"
           << "  \"recall\": " << result.recall << ",\n"
           << "  \"f1\": " << result.f1 << "\n"
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
    std::vector<std::string> image_ids;
    std::vector<p2::PersonGroundTruth> annotations;
    std::vector<p2::PersonPrediction> predictions;
    std::string error;
    if (!read_manifest(options.manifest, &image_ids, &error) ||
        !read_annotations(options.annotations, &annotations, &error) ||
        !read_predictions(options.predictions, &predictions, &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    p2::PersonEvaluationConfig config;
    config.confidence_threshold = options.confidence_threshold;
    config.iou_threshold = options.iou_threshold;
    p2::PersonEvaluationResult result;
    if (!p2::evaluate_person_predictions(image_ids, annotations, predictions,
                                         config, &result, &error)) {
        std::cerr << "evaluation failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    const bool passed = result.precision >= options.minimum_precision &&
        result.recall >= options.minimum_recall;
    write_json(std::cout, options, result, passed);
    if (!options.json_output.empty()) {
        std::ofstream json(options.json_output);
        if (!json) {
            std::cerr << "cannot write evaluation JSON\n";
            return EXIT_FAILURE;
        }
        write_json(json, options, result, passed);
    }
    if (!options.failures_output.empty()) {
        std::ofstream failures(options.failures_output);
        if (!failures) {
            std::cerr << "cannot write failure CSV\n";
            return EXIT_FAILURE;
        }
        failures << "image_id,ground_truth,predictions,true_positives,"
                    "false_positives,false_negatives\n";
        for (const p2::PersonImageEvaluation &image : result.per_image) {
            if (image.false_positives == 0 && image.false_negatives == 0)
                continue;
            failures << image.image_id << ',' << image.ground_truth << ','
                     << image.predictions << ',' << image.true_positives << ','
                     << image.false_positives << ',' << image.false_negatives
                     << '\n';
        }
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
