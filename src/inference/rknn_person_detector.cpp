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
#include <unordered_map>
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
        for (const auto &entry : dma_buf_handles)
            releasebuffer_handle(entry.second.handle);
        if (rgb_input_handle != 0)
            releasebuffer_handle(rgb_input_handle);
        if (io_input_rga_handle != 0)
            releasebuffer_handle(io_input_rga_handle);
        if (io_input_mem != nullptr)
            rknn_destroy_mem(context, io_input_mem);
        if (context != 0)
            rknn_destroy(context);
    }

    PersonDetectorConfig config;
    PersonDetectorRuntimeInfo info;
    rknn_context context = 0;
    std::vector<rknn_tensor_attr> input_attributes;
    std::vector<rknn_tensor_attr> output_attributes;
    std::vector<std::uint8_t> rgb_input;
    rga_buffer_handle_t rgb_input_handle = 0;
    rknn_tensor_mem *io_input_mem = nullptr;
    rga_buffer_handle_t io_input_rga_handle = 0;
    struct ImportedDmaBuffer {
        rga_buffer_handle_t handle = 0;
        std::size_t size = 0;
    };
    std::unordered_map<int, ImportedDmaBuffer> dma_buf_handles;
    bool initialized = false;

    bool infer_source(rga_buffer_t source,
                      bool source_uses_handle,
                      std::uint32_t width,
                      std::uint32_t height,
                      PersonInferenceResult *result,
                      std::string *error);
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
    if (impl_->config.use_io_mem) {
        rknn_tensor_attr &io_input = impl_->input_attributes.front();
        io_input.type = RKNN_TENSOR_UINT8;
        io_input.fmt = RKNN_TENSOR_NHWC;
        io_input.pass_through = 0;
        io_input.h_stride = 0;
        const std::uint32_t allocation_size = io_input.size_with_stride != 0U
            ? io_input.size_with_stride
            : static_cast<std::uint32_t>(input_bytes);
        impl_->io_input_mem = rknn_create_mem2(
            impl_->context, allocation_size,
            RKNN_FLAG_MEMORY_NON_CACHEABLE);
        if (impl_->io_input_mem == nullptr) {
            if (error != nullptr)
                *error = "rknn_create_mem input failed";
            return false;
        }
        impl_->io_input_rga_handle = importbuffer_fd(
            impl_->io_input_mem->fd,
            static_cast<int>(impl_->io_input_mem->size));
        if (impl_->io_input_rga_handle == 0) {
            if (error != nullptr)
                *error = "RGA import RKNN input fd failed";
            return false;
        }
        std::memset(impl_->io_input_mem->virt_addr, 114,
                    impl_->io_input_mem->size);
        status = rknn_set_io_mem(impl_->context, impl_->io_input_mem,
                                 &io_input);
        if (status < 0) {
            if (error != nullptr)
                *error = rknn_error("rknn_set_io_mem input", status);
            return false;
        }
        impl_->info.io_mem_enabled = true;
        impl_->info.input_width_stride = io_input.w_stride != 0U
            ? io_input.w_stride
            : impl_->info.model_width;
        impl_->info.input_size_with_stride = allocation_size;
        impl_->info.input_non_cacheable = true;
    } else {
        impl_->rgb_input.assign(input_bytes, 114U);
        impl_->info.input_width_stride = impl_->info.model_width;
        impl_->info.input_size_with_stride =
            static_cast<std::uint32_t>(input_bytes);
    }
    impl_->initialized = true;
    return true;
}

bool RknnPersonDetector::Impl::infer_source(
    rga_buffer_t source,
    bool source_uses_handle,
    std::uint32_t width,
    std::uint32_t height,
    PersonInferenceResult *result,
    std::string *error)
{
    const Clock::time_point total_begin = Clock::now();
    LetterboxTransform transform;
    if (!make_letterbox_transform(width, height, info.model_width,
                                  info.model_height, config.rotation,
                                  &transform, error))
        return false;

    if (!config.use_io_mem)
        std::fill(rgb_input.begin(), rgb_input.end(), 114U);
    rga_buffer_t destination{};
    if (config.use_io_mem) {
        destination = wrapbuffer_handle_t(
            io_input_rga_handle, static_cast<int>(info.model_width),
            static_cast<int>(info.model_height),
            static_cast<int>(info.input_width_stride),
            static_cast<int>(info.model_height), RK_FORMAT_RGB_888);
    } else if (source_uses_handle) {
        destination = wrapbuffer_handle_t(
            rgb_input_handle, static_cast<int>(info.model_width),
            static_cast<int>(info.model_height),
            static_cast<int>(info.model_width),
            static_cast<int>(info.model_height), RK_FORMAT_RGB_888);
    } else {
        destination = wrapbuffer_virtualaddr_t(
            rgb_input.data(), static_cast<int>(info.model_width),
            static_cast<int>(info.model_height),
            static_cast<int>(info.model_width),
            static_cast<int>(info.model_height), RK_FORMAT_RGB_888);
    }
    im_rect source_rect{0, 0, static_cast<int>(width),
                        static_cast<int>(height)};
    im_rect destination_rect{
        static_cast<int>(transform.pad_left),
        static_cast<int>(transform.pad_top),
        static_cast<int>(transform.resized_width),
        static_cast<int>(transform.resized_height)};
    const int usage = rga_rotation_usage(config.rotation);
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

    int status = 0;
    if (!config.use_io_mem) {
        rknn_input input{};
        input.index = 0;
        input.buf = rgb_input.data();
        input.size = static_cast<std::uint32_t>(rgb_input.size());
        input.type = RKNN_TENSOR_UINT8;
        input.fmt = RKNN_TENSOR_NHWC;
        input.pass_through = 0;
        status = rknn_inputs_set(context, 1U, &input);
        if (status < 0) {
            if (error != nullptr)
                *error = rknn_error("rknn_inputs_set", status);
            return false;
        }
    }

    std::vector<rknn_output> outputs(output_attributes.size());
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        outputs[index].index = static_cast<std::uint32_t>(index);
        outputs[index].want_float = 0;
    }
    const Clock::time_point rknn_begin = Clock::now();
    status = rknn_run(context, nullptr);
    if (status >= 0)
        status = rknn_outputs_get(context,
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
        const rknn_tensor_attr &attribute = output_attributes[index];
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
        views, transform, config.confidence_threshold,
        config.nms_threshold, config.maximum_detections,
        &detections, error);
    const Clock::time_point post_end = Clock::now();
    const int release_status = rknn_outputs_release(
        context, static_cast<std::uint32_t>(outputs.size()),
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
    result->timing.rga_import_ms = 0.0;
    result->timing.rga_ms = elapsed_ms(rga_begin, rga_end);
    result->timing.rknn_ms = elapsed_ms(rknn_begin, rknn_end);
    result->timing.postprocess_ms = elapsed_ms(post_begin, post_end);
    result->timing.total_ms = elapsed_ms(total_begin, Clock::now());
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
    if (!impl_->initialized || impl_->config.use_io_mem || data == nullptr ||
        result == nullptr ||
        bytes_per_line < width || height == 0U ||
        bytes_per_line > std::numeric_limits<std::size_t>::max() / height ||
        size < static_cast<std::size_t>(bytes_per_line) * height * 3U / 2U) {
        if (error != nullptr)
            *error = "invalid NV12 frame or uninitialized detector";
        return false;
    }
    rga_buffer_t source = wrapbuffer_virtualaddr_t(
        const_cast<std::uint8_t *>(data), static_cast<int>(width),
        static_cast<int>(height), static_cast<int>(bytes_per_line),
        static_cast<int>(height), RK_FORMAT_YCbCr_420_SP);
    return impl_->infer_source(source, false, width, height, result, error);
}

bool RknnPersonDetector::infer_nv12_dmabuf(
    int dma_buf_fd,
    std::size_t size,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t bytes_per_line,
    PersonInferenceResult *result,
    std::string *error)
{
    if (!impl_->initialized || dma_buf_fd < 0 || result == nullptr ||
        bytes_per_line < width || height == 0U ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        bytes_per_line > std::numeric_limits<std::size_t>::max() / height ||
        size < static_cast<std::size_t>(bytes_per_line) * height * 3U / 2U) {
        if (error != nullptr)
            *error = "invalid NV12 DMA-BUF or uninitialized detector";
        return false;
    }

    const Clock::time_point import_begin = Clock::now();
    if (!impl_->config.use_io_mem && impl_->rgb_input_handle == 0) {
        impl_->rgb_input_handle = importbuffer_virtualaddr(
            impl_->rgb_input.data(), static_cast<int>(impl_->rgb_input.size()));
        if (impl_->rgb_input_handle == 0) {
            if (error != nullptr)
                *error = "RGA importbuffer_virtualaddr destination failed";
            return false;
        }
    }
    auto found = impl_->dma_buf_handles.find(dma_buf_fd);
    if (found == impl_->dma_buf_handles.end()) {
        const rga_buffer_handle_t handle = importbuffer_fd(
            dma_buf_fd, static_cast<int>(size));
        if (handle == 0) {
            if (error != nullptr)
                *error = "RGA importbuffer_fd failed";
            return false;
        }
        Impl::ImportedDmaBuffer imported;
        imported.handle = handle;
        imported.size = size;
        found = impl_->dma_buf_handles.emplace(dma_buf_fd, imported).first;
        ++impl_->info.dma_buf_import_count;
    } else if (found->second.size != size) {
        if (error != nullptr)
            *error = "DMA-BUF fd was reused with a different size";
        return false;
    }
    rga_buffer_t source = wrapbuffer_handle_t(
        found->second.handle, static_cast<int>(width),
        static_cast<int>(height), static_cast<int>(bytes_per_line),
        static_cast<int>(height), RK_FORMAT_YCbCr_420_SP);
    const Clock::time_point import_end = Clock::now();
    if (!impl_->infer_source(source, true, width, height, result, error))
        return false;
    result->timing.rga_import_ms = elapsed_ms(import_begin, import_end);
    return true;
}

const PersonDetectorRuntimeInfo &RknnPersonDetector::runtime_info() const
{
    return impl_->info;
}

}  // namespace p2
