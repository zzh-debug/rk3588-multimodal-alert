#include "p2/streaming/rtsp_publisher.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

#include <cstring>
#include <utility>

namespace p2 {
namespace {

std::string ffmpeg_error(const char *operation, int status)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, buffer, sizeof(buffer));
    return std::string(operation) + " failed: " + buffer;
}

}  // namespace

struct RtspPublisher::Impl {
    explicit Impl(RtspPublisherConfig publisher_config)
        : config(std::move(publisher_config))
    {
    }

    RtspPublisherConfig config;
    RtspPublisherStats stats;
    AVFormatContext *format = nullptr;
    AVStream *stream = nullptr;
    bool header_written = false;
};

RtspPublisher::RtspPublisher(RtspPublisherConfig config)
    : impl_(new Impl(std::move(config)))
{
}

RtspPublisher::~RtspPublisher()
{
    close();
}

bool RtspPublisher::connect(
    const std::vector<std::uint8_t> &annex_b_header,
    std::string *error)
{
    if (impl_->header_written)
        return true;
    if (impl_->config.url.empty() || impl_->config.width == 0U ||
        impl_->config.height == 0U || impl_->config.fps == 0U ||
        annex_b_header.empty()) {
        if (error != nullptr)
            *error = "invalid RTSP publisher configuration or H.264 header";
        return false;
    }
    avformat_network_init();
    int status = avformat_alloc_output_context2(
        &impl_->format, nullptr, "rtsp", impl_->config.url.c_str());
    if (status < 0 || impl_->format == nullptr) {
        if (error != nullptr)
            *error = ffmpeg_error("RTSP output allocation", status);
        return false;
    }
    impl_->stream = avformat_new_stream(impl_->format, nullptr);
    if (impl_->stream == nullptr) {
        if (error != nullptr)
            *error = "avformat_new_stream failed";
        close();
        return false;
    }
    impl_->stream->id = 0;
    impl_->stream->time_base = {1, static_cast<int>(impl_->config.fps)};
    AVCodecParameters *parameters = impl_->stream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = AV_CODEC_ID_H264;
    parameters->codec_tag = 0;
    parameters->format = AV_PIX_FMT_YUV420P;
    parameters->width = static_cast<int>(impl_->config.width);
    parameters->height = static_cast<int>(impl_->config.height);
    parameters->extradata = static_cast<std::uint8_t *>(av_mallocz(
        annex_b_header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (parameters->extradata == nullptr) {
        if (error != nullptr)
            *error = "FFmpeg H.264 extradata allocation failed";
        close();
        return false;
    }
    std::memcpy(parameters->extradata, annex_b_header.data(),
                annex_b_header.size());
    parameters->extradata_size = static_cast<int>(annex_b_header.size());

    if ((impl_->format->oformat->flags & AVFMT_NOFILE) == 0) {
        status = avio_open2(&impl_->format->pb, impl_->config.url.c_str(),
                            AVIO_FLAG_WRITE, nullptr, nullptr);
        if (status < 0) {
            if (error != nullptr)
                *error = ffmpeg_error("RTSP avio_open2", status);
            close();
            return false;
        }
    }
    AVDictionary *options = nullptr;
    if (impl_->config.tcp_transport)
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "pkt_size", "1200", 0);
    status = avformat_write_header(impl_->format, &options);
    av_dict_free(&options);
    if (status < 0) {
        if (error != nullptr)
            *error = ffmpeg_error("RTSP avformat_write_header", status);
        close();
        return false;
    }
    impl_->header_written = true;
    return true;
}

bool RtspPublisher::publish(const EncodedH264Packet &packet,
                            std::string *error)
{
    if (!impl_->header_written || impl_->format == nullptr ||
        impl_->stream == nullptr || packet.bytes.empty()) {
        ++impl_->stats.failures;
        if (error != nullptr)
            *error = "RTSP publisher is disconnected or packet is empty";
        return false;
    }
    AVPacket *output = av_packet_alloc();
    if (output == nullptr ||
        av_new_packet(output, static_cast<int>(packet.bytes.size())) < 0) {
        if (output != nullptr)
            av_packet_free(&output);
        ++impl_->stats.failures;
        if (error != nullptr)
            *error = "FFmpeg packet allocation failed";
        return false;
    }
    std::memcpy(output->data, packet.bytes.data(), packet.bytes.size());
    output->stream_index = impl_->stream->index;
    const AVRational encoder_time_base{
        1, static_cast<int>(impl_->config.fps)};
    output->pts = av_rescale_q(packet.pts, encoder_time_base,
                               impl_->stream->time_base);
    output->dts = output->pts;
    output->duration = av_rescale_q(1, encoder_time_base,
                                    impl_->stream->time_base);
    output->pos = -1;
    if (packet.key_frame)
        output->flags |= AV_PKT_FLAG_KEY;
    const int status = av_interleaved_write_frame(impl_->format, output);
    av_packet_free(&output);
    if (status < 0) {
        ++impl_->stats.failures;
        if (error != nullptr)
            *error = ffmpeg_error("RTSP packet publish", status);
        return false;
    }
    ++impl_->stats.packets;
    impl_->stats.bytes += packet.bytes.size();
    return true;
}

void RtspPublisher::close()
{
    if (impl_->format == nullptr)
        return;
    if (impl_->header_written)
        av_write_trailer(impl_->format);
    if ((impl_->format->oformat->flags & AVFMT_NOFILE) == 0 &&
        impl_->format->pb != nullptr)
        avio_closep(&impl_->format->pb);
    avformat_free_context(impl_->format);
    impl_->format = nullptr;
    impl_->stream = nullptr;
    impl_->header_written = false;
}

bool RtspPublisher::connected() const
{
    return impl_->header_written;
}

const RtspPublisherStats &RtspPublisher::stats() const
{
    return impl_->stats;
}

}  // namespace p2
