#include "p2/inference/person_detector.hpp"

#include <im2d.h>
#include <rga.h>
#include <rknn_api.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace p2 {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

bool read_binary_file(const std::string &path,
                      std::vector<std::uint8_t> *bytes,
                      std::string *error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr)
            *error = "cannot open RKNN model: " + path;
        return false;
    }
    bytes->assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    if ((!input.good() && !input.eof()) || bytes->empty()) {
        if (error != nullptr)
            *error = "cannot read RKNN model: " + path;
        return false;
    }
    return true;
}

int rga_rotation_usage(ImageRotation rotation)
{
    switch (rotation) {
    case ImageRotation::kNone:
        return IM_SYNC;
    case ImageRotation::kClockwise90:
        return IM_SYNC | IM_HAL_TRANSFORM_ROT_90;
    case ImageRotation::kClockwise180:
        return IM_SYNC | IM_HAL_TRANSFORM_ROT_180;
    case ImageRotation::kClockwise270:
        return IM_SYNC | IM_HAL_TRANSFORM_ROT_270;
    }
    return IM_SYNC;
}

std::string rknn_error(const char *operation, int status)
{
    return std::string(operation) + " failed, status=" +
        std::to_string(status);
}

}  // namespace

struct RknnPersonDetector::Impl {
    explicit Impl(PersonDetectorConfig detector_config)
        : config(std::move(detector_config))
    {
    }

    ~Impl()
    {
        if (context != 0)
            rknn_destroy(context);
    }

    PersonDetectorConfig config;
    PersonDetectorRuntimeInfo info;
    rknn_context context = 0;
    std::vector<rknn_tensor_attr> input_attributes;
    std::vector<rknn_tensor_attr> output_attributes;
    std::vector<std::uint8_t> rgb_input;
    bool initialized = false;
};

RknnPersonDetector::RknnPersonDetector(PersonDetectorConfig config)
    : impl_(new Impl(std::move(config)))
{
}

RknnPersonDetector::~RknnPersonDetector() = default;

bool RknnPersonDetector::initialize(std::string *error)
{
    if (impl_->initialized)
        return true;
    if (impl_->config.model_path.empty()) {
        if (error != nullptr)
            *error = "RKNN model path is empty";
        return false;
    }

    std::vector<std::uint8_t> model;
    if (!read_binary_file(impl_->config.model_path, &model, error))
        return false;
    int status = rknn_init(&impl_->context, model.data(), model.size(), 0,
                           nullptr);
    if (status < 0) {
        if (error != nullptr)
            *error = rknn_error("rknn_init", status);
        impl_->context = 0;
        return false;
    }

    rknn_sdk_version version{};
    status = rknn_query(impl_->context, RKNN_QUERY_SDK_VERSION, &version,
                        sizeof(version));
    if (status < 0) {
        if (error != nullptr)
            *error = rknn_error("RKNN_QUERY_SDK_VERSION", status);
        return false;
    }
    impl_->info.rknn_api_version = version.api_version;
    impl_->info.rknn_driver_version = version.drv_version;

    rknn_input_output_num io_count{};
    status = rknn_query(impl_->context, RKNN_QUERY_IN_OUT_NUM, &io_count,
                        sizeof(io_count));
    if (status < 0) {
        if (error != nullptr)
            *error = rknn_error("RKNN_QUERY_IN_OUT_NUM", status);
        return false;
    }
    if (io_count.n_input != 1U || io_count.n_output != 3U) {
        if (error != nullptr)
            *error = "expected one YOLOv5 input and three outputs";
        return false;
    }
    impl_->info.input_count = io_count.n_input;
    impl_->info.output_count = io_count.n_output;

    impl_->input_attributes.resize(io_count.n_input);
    for (std::uint32_t index = 0; index < io_count.n_input; ++index) {
        rknn_tensor_attr &attribute = impl_->input_attributes[index];
        std::memset(&attribute, 0, sizeof(attribute));
        attribute.index = index;
        status = rknn_query(impl_->context, RKNN_QUERY_INPUT_ATTR,
                            &attribute, sizeof(attribute));
        if (status < 0) {
            if (error != nullptr)
                *error = rknn_error("RKNN_QUERY_INPUT_ATTR", status);
            return false;
        }
    }
    const rknn_tensor_attr &input = impl_->input_attributes.front();
    if (input.n_dims != 4U ||
        (input.type != RKNN_TENSOR_UINT8 &&
         input.type != RKNN_TENSOR_INT8)) {
        if (error != nullptr)
            *error = "expected a four-dimensional INT8/UINT8 model input";
        return false;
    }
    if (input.fmt == RKNN_TENSOR_NHWC) {
        impl_->info.model_height = input.dims[1];
        impl_->info.model_width = input.dims[2];
        impl_->info.model_channels = input.dims[3];
    } else if (input.fmt == RKNN_TENSOR_NCHW) {
        impl_->info.model_channels = input.dims[1];
        impl_->info.model_height = input.dims[2];
        impl_->info.model_width = input.dims[3];
    } else {
        if (error != nullptr)
            *error = "unsupported RKNN model input layout";
        return false;
    }
    if (impl_->info.model_width != 640U ||
        impl_->info.model_height != 640U ||
        impl_->info.model_channels != 3U) {
        if (error != nullptr)
            *error = "expected the SDK YOLOv5s 640x640 RGB model";
        return false;
    }

    impl_->output_attributes.resize(io_count.n_output);
    for (std::uint32_t index = 0; index < io_count.n_output; ++index) {
        rknn_tensor_attr &attribute = impl_->output_attributes[index];
        std::memset(&attribute, 0, sizeof(attribute));
        attribute.index = index;
        status = rknn_query(impl_->context, RKNN_QUERY_OUTPUT_ATTR,
                            &attribute, sizeof(attribute));
        if (status < 0) {
            if (error != nullptr)
                *error = rknn_error("RKNN_QUERY_OUTPUT_ATTR", status);
            return false;
        }
        if (attribute.n_dims != 4U || attribute.fmt != RKNN_TENSOR_NCHW ||
            attribute.type != RKNN_TENSOR_INT8 ||
            attribute.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
            attribute.dims[1] != 255U) {
            if (error != nullptr)
                *error = "unsupported YOLOv5 output tensor contract";
            return false;
        }
    }
    const std::size_t input_bytes =
        static_cast<std::size_t>(impl_->info.model_width) *
        impl_->info.model_height * impl_->info.model_channels;
    impl_->rgb_input.assign(input_bytes, 114U);
    impl_->initialized = true;
    return true;
}

bool RknnPersonDetector::infer_nv12(const std::uint8_t *data,
                                    std::size_t size,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    std::uint32_t bytes_per_line,
                                    PersonInferenceResult *result,
                                    std::string *error)
{
    if (!impl_->initialized || data == nullptr || result == nullptr ||
        bytes_per_line < width || height == 0U ||
        bytes_per_line > std::numeric_limits<std::size_t>::max() / height ||
        size < static_cast<std::size_t>(bytes_per_line) * height * 3U / 2U) {
        if (error != nullptr)
            *error = "invalid NV12 frame or uninitialized detector";
        return false;
    }

    const Clock::time_point total_begin = Clock::now();
    LetterboxTransform transform;
    if (!make_letterbox_transform(width, height, impl_->info.model_width,
                                  impl_->info.model_height,
                                  impl_->config.rotation, &transform, error))
        return false;

    std::fill(impl_->rgb_input.begin(), impl_->rgb_input.end(), 114U);
    rga_buffer_t source = wrapbuffer_virtualaddr_t(
        const_cast<std::uint8_t *>(data), static_cast<int>(width),
        static_cast<int>(height), static_cast<int>(bytes_per_line),
        static_cast<int>(height), RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t destination = wrapbuffer_virtualaddr_t(
        impl_->rgb_input.data(), static_cast<int>(impl_->info.model_width),
        static_cast<int>(impl_->info.model_height),
        static_cast<int>(impl_->info.model_width),
        static_cast<int>(impl_->info.model_height), RK_FORMAT_RGB_888);
    im_rect source_rect{0, 0, static_cast<int>(width),
                        static_cast<int>(height)};
    im_rect destination_rect{
        static_cast<int>(transform.pad_left),
        static_cast<int>(transform.pad_top),
        static_cast<int>(transform.resized_width),
        static_cast<int>(transform.resized_height)};
    const int usage = rga_rotation_usage(impl_->config.rotation);
    IM_STATUS rga_status = imcheck(source, destination, source_rect,
                                   destination_rect, usage);
    if (rga_status != IM_STATUS_NOERROR) {
        if (error != nullptr)
            *error = std::string("RGA imcheck failed: ") +
                imStrError(rga_status);
        return false;
    }
    const Clock::time_point rga_begin = Clock::now();
    rga_status = improcess(source, destination, {}, source_rect,
                           destination_rect, {}, usage);
    const Clock::time_point rga_end = Clock::now();
    if (rga_status != IM_STATUS_SUCCESS) {
        if (error != nullptr)
            *error = std::string("RGA letterbox failed: ") +
                imStrError(rga_status);
        return false;
    }

    rknn_input input{};
    input.index = 0;
    input.buf = impl_->rgb_input.data();
    input.size = static_cast<std::uint32_t>(impl_->rgb_input.size());
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.pass_through = 0;
    int status = rknn_inputs_set(impl_->context, 1U, &input);
    if (status < 0) {
        if (error != nullptr)
            *error = rknn_error("rknn_inputs_set", status);
        return false;
    }

    std::vector<rknn_output> outputs(impl_->output_attributes.size());
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        outputs[index].index = static_cast<std::uint32_t>(index);
        outputs[index].want_float = 0;
    }
    const Clock::time_point rknn_begin = Clock::now();
    status = rknn_run(impl_->context, nullptr);
    if (status >= 0)
        status = rknn_outputs_get(impl_->context,
                                  static_cast<std::uint32_t>(outputs.size()),
                                  outputs.data(), nullptr);
    const Clock::time_point rknn_end = Clock::now();
    if (status < 0) {
        if (error != nullptr)
            *error = rknn_error("RKNN inference", status);
        return false;
    }

    std::vector<QuantizedYoloTensorView> views;
    views.reserve(outputs.size());
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        const rknn_tensor_attr &attribute = impl_->output_attributes[index];
        QuantizedYoloTensorView view;
        view.data = static_cast<const std::int8_t *>(outputs[index].buf);
        view.size = outputs[index].size;
        view.grid_height = attribute.dims[2];
        view.grid_width = attribute.dims[3];
        view.zero_point = attribute.zp;
        view.scale = attribute.scale;
        views.push_back(view);
    }
    const Clock::time_point post_begin = Clock::now();
    std::vector<PersonDetection> detections;
    const bool decoded = decode_yolov5_person(
        views, transform, impl_->config.confidence_threshold,
        impl_->config.nms_threshold, impl_->config.maximum_detections,
        &detections, error);
    const Clock::time_point post_end = Clock::now();
    const int release_status = rknn_outputs_release(
        impl_->context, static_cast<std::uint32_t>(outputs.size()),
        outputs.data());
    if (!decoded)
        return false;
    if (release_status < 0) {
        if (error != nullptr)
            *error = rknn_error("rknn_outputs_release", release_status);
        return false;
    }

    result->transform = transform;
    result->detections = std::move(detections);
    result->timing.rga_ms = elapsed_ms(rga_begin, rga_end);
    result->timing.rknn_ms = elapsed_ms(rknn_begin, rknn_end);
    result->timing.postprocess_ms = elapsed_ms(post_begin, post_end);
    result->timing.total_ms = elapsed_ms(total_begin, Clock::now());
    return true;
}

const PersonDetectorRuntimeInfo &RknnPersonDetector::runtime_info() const
{
    return impl_->info;
}

}  // namespace p2
