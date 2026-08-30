#include "p2/streaming/mpp_h264_encoder.hpp"

#include <im2d.h>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_meta.h>
#include <mpp_packet.h>
#include <rga.h>
#include <rk_mpi.h>
#include <rk_venc_cmd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>

namespace p2 {
namespace {

using Clock = std::chrono::steady_clock;

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

int align_down_int(int value, int alignment)
{
    return value / alignment * alignment;
}

int align_up_int(int value, int alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

double elapsed_ms(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
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

std::array<std::uint8_t, 7> glyph(char character)
{
    switch (character) {
    case '0': return {14, 17, 19, 21, 25, 17, 14};
    case '1': return {4, 12, 4, 4, 4, 4, 14};
    case '2': return {14, 17, 1, 2, 4, 8, 31};
    case '3': return {30, 1, 1, 14, 1, 1, 30};
    case '4': return {2, 6, 10, 18, 31, 2, 2};
    case '5': return {31, 16, 16, 30, 1, 1, 30};
    case '6': return {14, 16, 16, 30, 17, 17, 14};
    case '7': return {31, 1, 2, 4, 8, 8, 8};
    case '8': return {14, 17, 17, 14, 17, 17, 14};
    case '9': return {14, 17, 17, 15, 1, 1, 14};
    case 'A': return {14, 17, 17, 31, 17, 17, 17};
    case 'C': return {14, 17, 16, 16, 16, 17, 14};
    case 'E': return {31, 16, 16, 30, 16, 16, 31};
    case 'I': return {14, 4, 4, 4, 4, 4, 14};
    case 'L': return {16, 16, 16, 16, 16, 16, 31};
    case 'M': return {17, 27, 21, 21, 17, 17, 17};
    case 'N': return {17, 25, 25, 21, 19, 19, 17};
    case 'O': return {14, 17, 17, 17, 17, 17, 14};
    case 'P': return {30, 17, 17, 30, 16, 16, 16};
    case 'R': return {30, 17, 17, 30, 20, 18, 17};
    case 'S': return {15, 16, 16, 14, 1, 1, 30};
    case 'T': return {31, 4, 4, 4, 4, 4, 4};
    case '%': return {17, 2, 4, 8, 17, 0, 0};
    case '.': return {0, 0, 0, 0, 0, 12, 12};
    case ':': return {0, 12, 12, 0, 12, 12, 0};
    case '-': return {0, 0, 0, 31, 0, 0, 0};
    default: return {0, 0, 0, 0, 0, 0, 0};
    }
}

void fill_rect(std::uint8_t *pixels, int width, int height,
               int left, int top, int right, int bottom,
               std::uint8_t color)
{
    left = std::clamp(left, 0, width);
    right = std::clamp(right, 0, width);
    top = std::clamp(top, 0, height);
    bottom = std::clamp(bottom, 0, height);
    if (right <= left || bottom <= top)
        return;
    for (int y = top; y < bottom; ++y)
        std::fill(pixels + y * width + left,
                  pixels + y * width + right, color);
}

void draw_text(std::uint8_t *pixels, int width, int height,
               int left, int top, const std::string &text,
               std::uint8_t color)
{
    constexpr int scale = 2;
    int cursor = left;
    for (char character : text) {
        if (character == ' ') {
            cursor += 6 * scale;
            continue;
        }
        const std::array<std::uint8_t, 7> rows = glyph(character);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((rows[static_cast<std::size_t>(row)] &
                     (1U << (4 - column))) == 0U)
                    continue;
                fill_rect(pixels, width, height,
                          cursor + column * scale, top + row * scale,
                          cursor + (column + 1) * scale,
                          top + (row + 1) * scale, color);
            }
        }
        cursor += 6 * scale;
        if (cursor >= width)
            break;
    }
}

}  // namespace

struct MppH264Encoder::Impl {
    explicit Impl(MppH264EncoderConfig encoder_config)
        : config(std::move(encoder_config))
    {
    }

    ~Impl()
    {
        for (const auto &entry : source_handles)
            releasebuffer_handle(entry.second.handle);
        if (destination_handle != 0)
            releasebuffer_handle(destination_handle);
        if (context != nullptr && api != nullptr)
            api->reset(context);
        if (context != nullptr)
            mpp_destroy(context);
        if (encoder_config != nullptr)
            mpp_enc_cfg_deinit(encoder_config);
        if (osd_buffer != nullptr)
            mpp_buffer_put(osd_buffer);
        if (packet_buffer != nullptr)
            mpp_buffer_put(packet_buffer);
        if (frame_buffer != nullptr)
            mpp_buffer_put(frame_buffer);
        if (buffer_group != nullptr)
            mpp_buffer_group_put(buffer_group);
    }

    struct ImportedSource {
        rga_buffer_handle_t handle = 0;
        std::size_t size = 0;
    };

    MppH264EncoderConfig config;
    MppH264EncoderStats stats;
    std::vector<std::uint8_t> header;
    MppCtx context = nullptr;
    MppApi *api = nullptr;
    MppEncCfg encoder_config = nullptr;
    MppBufferGroup buffer_group = nullptr;
    MppBuffer frame_buffer = nullptr;
    MppBuffer packet_buffer = nullptr;
    MppBuffer osd_buffer = nullptr;
    rga_buffer_handle_t destination_handle = 0;
    std::unordered_map<int, ImportedSource> source_handles;
    std::uint32_t horizontal_stride = 0;
    std::uint32_t vertical_stride = 0;
    std::size_t frame_size = 0;
    std::size_t osd_size = 0;
    MppEncOSDPlt palette{};
    MppEncOSDPltCfg palette_config{};
    MppEncOSDData osd_data{};
    bool initialized = false;

    bool configure(std::string *error);
    bool prepare_osd(const VideoOverlay &overlay, std::string *error);
    bool add_osd_region(int absolute_left, int absolute_top,
                        int absolute_right, int absolute_bottom,
                        const OverlayBox *box,
                        const std::string &label,
                        OverlayColor color,
                        std::size_t *next_offset,
                        std::string *error);
};

MppH264Encoder::MppH264Encoder(MppH264EncoderConfig config)
    : impl_(new Impl(std::move(config)))
{
}

MppH264Encoder::~MppH264Encoder() = default;

bool MppH264Encoder::Impl::configure(std::string *error)
{
    const auto set_s32 = [&](const char *name, std::int32_t value) {
        return mpp_enc_cfg_set_s32(encoder_config, name, value) == MPP_OK;
    };
    bool ok = true;
    ok = set_s32("prep:width", static_cast<std::int32_t>(config.width)) && ok;
    ok = set_s32("prep:height", static_cast<std::int32_t>(config.height)) && ok;
    ok = set_s32("prep:hor_stride",
                 static_cast<std::int32_t>(horizontal_stride)) && ok;
    ok = set_s32("prep:ver_stride",
                 static_cast<std::int32_t>(vertical_stride)) && ok;
    ok = set_s32("prep:format", MPP_FMT_YUV420SP) && ok;
    ok = set_s32("rc:mode", MPP_ENC_RC_MODE_CBR) && ok;
    ok = set_s32("rc:fps_in_flex", 0) && ok;
    ok = set_s32("rc:fps_in_num", static_cast<std::int32_t>(config.fps)) && ok;
    ok = set_s32("rc:fps_in_denom", 1) && ok;
    ok = set_s32("rc:fps_out_flex", 0) && ok;
    ok = set_s32("rc:fps_out_num", static_cast<std::int32_t>(config.fps)) && ok;
    ok = set_s32("rc:fps_out_denom", 1) && ok;
    ok = set_s32("rc:bps_target",
                 static_cast<std::int32_t>(config.bitrate_bps)) && ok;
    ok = set_s32("rc:bps_max",
                 static_cast<std::int32_t>(config.bitrate_bps * 17U / 16U)) && ok;
    ok = set_s32("rc:bps_min",
                 static_cast<std::int32_t>(config.bitrate_bps * 15U / 16U)) && ok;
    ok = set_s32("rc:qp_init", -1) && ok;
    ok = set_s32("rc:qp_max", 51) && ok;
    ok = set_s32("rc:qp_min", 10) && ok;
    ok = set_s32("rc:qp_max_i", 51) && ok;
    ok = set_s32("rc:qp_min_i", 10) && ok;
    ok = set_s32("rc:qp_ip", 2) && ok;
    ok = set_s32("rc:gop", static_cast<std::int32_t>(config.gop_frames)) && ok;
    ok = set_s32("codec:type", MPP_VIDEO_CodingAVC) && ok;
    ok = set_s32("h264:profile", 100) && ok;
    ok = set_s32("h264:level", 40) && ok;
    ok = set_s32("h264:cabac_en", 1) && ok;
    ok = set_s32("h264:cabac_idc", 0) && ok;
    ok = set_s32("h264:trans8x8", 1) && ok;
    if (!ok) {
        if (error != nullptr)
            *error = "MPP rejected one or more H.264 configuration keys";
        return false;
    }
    if (api->control(context, MPP_ENC_SET_CFG, encoder_config) != MPP_OK) {
        if (error != nullptr)
            *error = "MPP_ENC_SET_CFG failed";
        return false;
    }
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    if (api->control(context, MPP_ENC_SET_HEADER_MODE, &header_mode) != MPP_OK) {
        if (error != nullptr)
            *error = "MPP_ENC_SET_HEADER_MODE failed";
        return false;
    }

    palette.data[0].val = MPP_ENC_OSD_PLT_TRANS;
    palette.data[1].val = MPP_ENC_OSD_PLT_GREEN;
    palette.data[2].val = MPP_ENC_OSD_PLT_RED;
    palette.data[3].val = MPP_ENC_OSD_PLT_YELLOW;
    palette.data[4].val = MPP_ENC_OSD_PLT_WHITE;
    palette.data[5].val = MPP_ENC_OSD_PLT_BLACK;
    for (std::size_t index = 6; index < 256; ++index)
        palette.data[index].val = MPP_ENC_OSD_PLT_TRANS;
    palette_config.change = MPP_ENC_OSD_PLT_CFG_CHANGE_ALL;
    palette_config.type = MPP_ENC_OSD_PLT_TYPE_USERDEF;
    palette_config.plt = &palette;
    if (api->control(context, MPP_ENC_SET_OSD_PLT_CFG,
                     &palette_config) != MPP_OK) {
        if (error != nullptr)
            *error = "MPP_ENC_SET_OSD_PLT_CFG failed";
        return false;
    }
    return true;
}

bool MppH264Encoder::initialize(std::string *error)
{
    if (impl_->initialized)
        return true;
    const MppH264EncoderConfig &config = impl_->config;
    if (config.width == 0U || config.height == 0U || config.fps == 0U ||
        config.bitrate_bps == 0U || config.gop_frames == 0U ||
        (config.width & 1U) != 0U || (config.height & 1U) != 0U) {
        if (error != nullptr)
            *error = "invalid MPP H.264 encoder configuration";
        return false;
    }
    impl_->horizontal_stride = align_up(config.width, 16U);
    impl_->vertical_stride = align_up(config.height, 16U);
    impl_->frame_size = static_cast<std::size_t>(
        align_up(impl_->horizontal_stride, 64U)) *
        align_up(impl_->vertical_stride, 64U) * 3U / 2U;
    impl_->osd_size = static_cast<std::size_t>(
        impl_->horizontal_stride) * impl_->vertical_stride;

    const MppBufferType buffer_type = static_cast<MppBufferType>(
        MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_CACHABLE);
    if (mpp_buffer_group_get_internal(&impl_->buffer_group,
                                      buffer_type) != MPP_OK ||
        mpp_buffer_get(impl_->buffer_group, &impl_->frame_buffer,
                       impl_->frame_size) != MPP_OK ||
        mpp_buffer_get(impl_->buffer_group, &impl_->packet_buffer,
                       impl_->frame_size) != MPP_OK ||
        mpp_buffer_get(impl_->buffer_group, &impl_->osd_buffer,
                       impl_->osd_size) != MPP_OK) {
        if (error != nullptr)
            *error = "MPP DRM buffer allocation failed";
        return false;
    }
    const int destination_fd = mpp_buffer_get_fd(impl_->frame_buffer);
    if (destination_fd < 0 ||
        impl_->frame_size > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        if (error != nullptr)
            *error = "MPP input buffer cannot be imported by RGA";
        return false;
    }
    impl_->destination_handle = importbuffer_fd(
        destination_fd, static_cast<int>(impl_->frame_size));
    if (impl_->destination_handle == 0) {
        if (error != nullptr)
            *error = "RGA import of MPP input DMA-BUF failed";
        return false;
    }

    if (mpp_create(&impl_->context, &impl_->api) != MPP_OK) {
        if (error != nullptr)
            *error = "mpp_create failed";
        return false;
    }
    MppPollType timeout = MPP_POLL_BLOCK;
    if (impl_->api->control(impl_->context, MPP_SET_OUTPUT_TIMEOUT,
                            &timeout) != MPP_OK ||
        mpp_init(impl_->context, MPP_CTX_ENC,
                 MPP_VIDEO_CodingAVC) != MPP_OK ||
        mpp_enc_cfg_init(&impl_->encoder_config) != MPP_OK ||
        impl_->api->control(impl_->context, MPP_ENC_GET_CFG,
                            impl_->encoder_config) != MPP_OK ||
        !impl_->configure(error)) {
        if (error != nullptr && error->empty())
            *error = "MPP encoder initialization failed";
        return false;
    }

    MppPacket header_packet = nullptr;
    if (mpp_packet_init_with_buffer(&header_packet,
                                    impl_->packet_buffer) != MPP_OK) {
        if (error != nullptr)
            *error = "MPP header packet initialization failed";
        return false;
    }
    mpp_packet_set_length(header_packet, 0);
    const MPP_RET header_status = impl_->api->control(
        impl_->context, MPP_ENC_GET_HDR_SYNC, header_packet);
    if (header_status == MPP_OK) {
        const auto *data = static_cast<const std::uint8_t *>(
            mpp_packet_get_pos(header_packet));
        const std::size_t length = mpp_packet_get_length(header_packet);
        if (data != nullptr && length != 0U)
            impl_->header.assign(data, data + length);
    }
    mpp_packet_deinit(&header_packet);
    if (header_status != MPP_OK || impl_->header.empty()) {
        if (error != nullptr)
            *error = "MPP did not return an H.264 SPS/PPS header";
        return false;
    }
    impl_->osd_data.buf = impl_->osd_buffer;
    impl_->initialized = true;
    return true;
}

bool MppH264Encoder::Impl::add_osd_region(
    int absolute_left, int absolute_top,
    int absolute_right, int absolute_bottom,
    const OverlayBox *box, const std::string &label,
    OverlayColor color, std::size_t *next_offset,
    std::string *error)
{
    if (osd_data.num_region >= 8U)
        return true;
    const int frame_width = static_cast<int>(config.width);
    const int frame_height = static_cast<int>(config.height);
    int left = std::clamp(align_down_int(absolute_left, 16), 0,
                          frame_width - 16);
    int top = std::clamp(align_down_int(absolute_top, 16), 0,
                         frame_height - 16);
    int right = std::clamp(align_up_int(absolute_right, 16), left + 16,
                           static_cast<int>(horizontal_stride));
    int bottom = std::clamp(align_up_int(absolute_bottom, 16), top + 16,
                            static_cast<int>(vertical_stride));
    const int region_width = right - left;
    const int region_height = bottom - top;
    const std::size_t region_size = static_cast<std::size_t>(region_width) *
        static_cast<std::size_t>(region_height);
    if (*next_offset > osd_size || region_size > osd_size - *next_offset) {
        if (error != nullptr)
            *error = "MPP OSD regions exceed the bounded overlay buffer";
        return false;
    }
    auto *base = static_cast<std::uint8_t *>(mpp_buffer_get_ptr(osd_buffer));
    if (base == nullptr) {
        if (error != nullptr)
            *error = "MPP OSD buffer is not CPU-addressable";
        return false;
    }
    std::uint8_t *pixels = base + *next_offset;
    std::memset(pixels, 0, region_size);
    const std::uint8_t palette_index = static_cast<std::uint8_t>(color);
    if (box == nullptr) {
        fill_rect(pixels, region_width, region_height,
                  0, 0, region_width, region_height, palette_index);
        draw_text(pixels, region_width, region_height, 8, 8, label, 4);
    } else {
        const int box_left = box->left - left;
        const int box_top = box->top - top;
        const int box_right = box->right - left;
        const int box_bottom = box->bottom - top;
        constexpr int border = 4;
        fill_rect(pixels, region_width, region_height,
                  box_left, box_top, box_right, box_top + border,
                  palette_index);
        fill_rect(pixels, region_width, region_height,
                  box_left, box_bottom - border, box_right, box_bottom,
                  palette_index);
        fill_rect(pixels, region_width, region_height,
                  box_left, box_top, box_left + border, box_bottom,
                  palette_index);
        fill_rect(pixels, region_width, region_height,
                  box_right - border, box_top, box_right, box_bottom,
                  palette_index);
        const int label_width = std::min(
            region_width, static_cast<int>(label.size()) * 12 + 16);
        const int label_top = std::max(0, box_top - 22);
        fill_rect(pixels, region_width, region_height,
                  box_left, label_top,
                  std::min(region_width, box_left + label_width),
                  std::min(region_height, label_top + 20), palette_index);
        draw_text(pixels, region_width, region_height,
                  box_left + 6, label_top + 3, label, 4);
    }

    MppEncOSDRegion &region = osd_data.region[osd_data.num_region++];
    std::memset(&region, 0, sizeof(region));
    region.enable = 1;
    region.inverse = 0;
    region.start_mb_x = static_cast<RK_U32>(left / 16);
    region.start_mb_y = static_cast<RK_U32>(top / 16);
    region.num_mb_x = static_cast<RK_U32>(region_width / 16);
    region.num_mb_y = static_cast<RK_U32>(region_height / 16);
    region.buf_offset = static_cast<RK_U32>(*next_offset);
    *next_offset += region_size;
    return true;
}

bool MppH264Encoder::Impl::prepare_osd(const VideoOverlay &overlay,
                                       std::string *error)
{
    std::memset(osd_data.region, 0, sizeof(osd_data.region));
    osd_data.num_region = 0;
    osd_data.buf = osd_buffer;
    std::size_t next_offset = 0;
    const int banner_width = std::clamp(
        static_cast<int>(overlay.banner.size()) * 12 + 24, 128,
        static_cast<int>(config.width) - 32);
    if (!add_osd_region(16, 16, 16 + banner_width, 64, nullptr,
                        overlay.banner, overlay.banner_color,
                        &next_offset, error))
        return false;
    for (const OverlayBox &box : overlay.boxes) {
        if (osd_data.num_region >= 8U)
            break;
        const int top = std::max(0, box.top - 24);
        if (!add_osd_region(box.left, top, box.right, box.bottom,
                            &box, box.label, box.color,
                            &next_offset, error))
            break;
    }
    mpp_buffer_sync_end(osd_buffer);
    return osd_data.num_region != 0U;
}

bool MppH264Encoder::encode_nv12_dmabuf(
    int dma_buf_fd, std::size_t size,
    std::uint32_t source_width, std::uint32_t source_height,
    std::uint32_t source_stride, std::uint64_t capture_timestamp_ns,
    const VideoOverlay &overlay, EncodedH264Packet *packet,
    std::string *error)
{
    if (!impl_->initialized || packet == nullptr || dma_buf_fd < 0 ||
        source_width == 0U || source_height == 0U ||
        source_stride < source_width ||
        size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        size < static_cast<std::size_t>(source_stride) *
            source_height * 3U / 2U) {
        if (error != nullptr)
            *error = "invalid NV12 DMA-BUF or uninitialized MPP encoder";
        return false;
    }
    auto found = impl_->source_handles.find(dma_buf_fd);
    if (found == impl_->source_handles.end()) {
        const rga_buffer_handle_t handle = importbuffer_fd(
            dma_buf_fd, static_cast<int>(size));
        if (handle == 0) {
            if (error != nullptr)
                *error = "RGA import of capture DMA-BUF failed";
            return false;
        }
        found = impl_->source_handles.emplace(
            dma_buf_fd, Impl::ImportedSource{handle, size}).first;
        ++impl_->stats.source_dma_buf_imports;
    } else if (found->second.size != size) {
        if (error != nullptr)
            *error = "capture DMA-BUF fd was reused with a different size";
        return false;
    }

    const rga_buffer_t source = wrapbuffer_handle_t(
        found->second.handle, static_cast<int>(source_width),
        static_cast<int>(source_height), static_cast<int>(source_stride),
        static_cast<int>(source_height), RK_FORMAT_YCbCr_420_SP);
    const rga_buffer_t destination = wrapbuffer_handle_t(
        impl_->destination_handle, static_cast<int>(impl_->config.width),
        static_cast<int>(impl_->config.height),
        static_cast<int>(impl_->horizontal_stride),
        static_cast<int>(impl_->vertical_stride),
        RK_FORMAT_YCbCr_420_SP);
    const im_rect source_rect{0, 0, static_cast<int>(source_width),
                              static_cast<int>(source_height)};
    const im_rect destination_rect{
        0, 0, static_cast<int>(impl_->config.width),
        static_cast<int>(impl_->config.height)};
    const int usage = rga_rotation_usage(impl_->config.rotation);
    IM_STATUS rga_status = imcheck(source, destination, source_rect,
                                   destination_rect, usage);
    if (rga_status != IM_STATUS_NOERROR) {
        if (error != nullptr)
            *error = std::string("RGA encoder imcheck failed: ") +
                imStrError(rga_status);
        return false;
    }
    const Clock::time_point rga_begin = Clock::now();
    rga_status = improcess(source, destination, {}, source_rect,
                           destination_rect, {}, usage);
    const Clock::time_point rga_end = Clock::now();
    if (rga_status != IM_STATUS_SUCCESS) {
        if (error != nullptr)
            *error = std::string("RGA encoder transform failed: ") +
                imStrError(rga_status);
        return false;
    }
    impl_->stats.rga_total_ms += elapsed_ms(rga_begin, rga_end);
    if (!impl_->prepare_osd(overlay, error))
        return false;

    MppFrame frame = nullptr;
    if (mpp_frame_init(&frame) != MPP_OK) {
        if (error != nullptr)
            *error = "mpp_frame_init failed";
        return false;
    }
    mpp_frame_set_width(frame, impl_->config.width);
    mpp_frame_set_height(frame, impl_->config.height);
    mpp_frame_set_hor_stride(frame, impl_->horizontal_stride);
    mpp_frame_set_ver_stride(frame, impl_->vertical_stride);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_pts(frame, static_cast<RK_S64>(capture_timestamp_ns));
    mpp_frame_set_buffer(frame, impl_->frame_buffer);
    MppMeta metadata = mpp_frame_get_meta(frame);
    mpp_meta_set_ptr(metadata, KEY_OSD_DATA, &impl_->osd_data);

    MppPacket output = nullptr;
    if (mpp_packet_init_with_buffer(&output,
                                    impl_->packet_buffer) != MPP_OK) {
        mpp_frame_deinit(&frame);
        if (error != nullptr)
            *error = "MPP output packet initialization failed";
        return false;
    }
    mpp_packet_set_length(output, 0);
    mpp_meta_set_packet(metadata, KEY_OUTPUT_PACKET, output);
    const Clock::time_point mpp_begin = Clock::now();
    MPP_RET status = impl_->api->encode_put_frame(impl_->context, frame);
    mpp_frame_deinit(&frame);
    if (status == MPP_OK)
        status = impl_->api->encode_get_packet(impl_->context, &output);
    const Clock::time_point mpp_end = Clock::now();
    if (status != MPP_OK || output == nullptr) {
        if (output != nullptr)
            mpp_packet_deinit(&output);
        if (error != nullptr)
            *error = "MPP H.264 frame encode failed";
        return false;
    }
    const auto *data = static_cast<const std::uint8_t *>(
        mpp_packet_get_pos(output));
    const std::size_t length = mpp_packet_get_length(output);
    if (data == nullptr || length == 0U) {
        mpp_packet_deinit(&output);
        if (error != nullptr)
            *error = "MPP returned an empty H.264 packet";
        return false;
    }
    packet->bytes.assign(data, data + length);
    packet->pts = static_cast<std::int64_t>(impl_->stats.encoded_frames);
    RK_S32 is_intra = 0;
    MppMeta packet_metadata = mpp_packet_get_meta(output);
    if (packet_metadata != nullptr)
        mpp_meta_get_s32(packet_metadata, KEY_OUTPUT_INTRA, &is_intra);
    packet->key_frame = is_intra != 0;
    mpp_packet_deinit(&output);
    ++impl_->stats.encoded_frames;
    impl_->stats.encoded_bytes += length;
    impl_->stats.key_frames += packet->key_frame ? 1U : 0U;
    ++impl_->stats.osd_frames;
    impl_->stats.mpp_total_ms += elapsed_ms(mpp_begin, mpp_end);
    return true;
}

const std::vector<std::uint8_t> &MppH264Encoder::codec_header() const
{
    return impl_->header;
}

const MppH264EncoderStats &MppH264Encoder::stats() const
{
    return impl_->stats;
}

}  // namespace p2
