#include "p2/capture/thermal_capture.hpp"

#include "p2/capture/v4l2_utils.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <linux/videodev2.h>
#include <linux/zzh_mlx90640_meta.h>

namespace p2 {
namespace {

struct MappedBuffer {
    void *address = nullptr;
    std::size_t length = 0;
};

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
std::uint16_t from_le16(__le16 value) { return value; }
std::uint32_t from_le32(__le32 value) { return value; }
std::uint64_t from_le64(__le64 value) { return value; }
#else
std::uint16_t from_le16(__le16 value) { return __builtin_bswap16(value); }
std::uint32_t from_le32(__le32 value) { return __builtin_bswap32(value); }
std::uint64_t from_le64(__le64 value) { return __builtin_bswap64(value); }
#endif

void set_error(std::string *error, const std::string &operation)
{
    if (error != nullptr)
        *error = operation + ": " + std::strerror(errno);
}

bool valid_meta(const zzh_mlx90640_meta_v1 &meta,
                ThermalFrameEvent *event)
{
    const std::uint32_t flags = from_le32(meta.flags);
    const std::uint16_t first_id = from_le16(meta.subpage[0].subpage_id);
    const std::uint16_t second_id = from_le16(meta.subpage[1].subpage_id);
    const std::uint64_t first_ready =
        from_le64(meta.subpage[0].ready_timestamp_ns);
    const std::uint64_t second_ready =
        from_le64(meta.subpage[1].ready_timestamp_ns);
    const std::uint64_t first_done =
        from_le64(meta.subpage[0].read_done_timestamp_ns);
    const std::uint64_t second_done =
        from_le64(meta.subpage[1].read_done_timestamp_ns);
    const std::uint32_t required_flags =
        ZZH_MLX90640_META_FLAG_CHESS |
        ZZH_MLX90640_META_FLAG_CONSECUTIVE;

    if (from_le32(meta.magic) != ZZH_MLX90640_META_MAGIC ||
        from_le16(meta.version) != ZZH_MLX90640_META_VERSION ||
        from_le16(meta.header_size) != ZZH_MLX90640_META_V1_HEADER_SIZE ||
        from_le32(meta.buffer_size) != ZZH_MLX90640_META_V1_SIZE ||
        (flags & required_flags) != required_flags || first_id > 1 ||
        second_id > 1 || first_id == second_id || first_ready == 0 ||
        second_ready <= first_ready || first_done < first_ready ||
        second_done < second_ready)
        return false;

    event->pair_sequence = from_le32(meta.pair_sequence);
    event->timestamp_ns = first_ready + (second_ready - first_ready) / 2;
    event->first_ready_ns = first_ready;
    event->second_ready_ns = second_ready;
    event->second_read_done_ns = second_done;
    event->flags = flags;
    return true;
}

}  // namespace

static_assert(sizeof(zzh_mlx90640_meta_v1) == ZZH_MLX90640_META_V1_SIZE,
              "ZMLX v1 ABI size mismatch");

ThermalCapture::ThermalCapture(ThermalCaptureConfig config)
    : config_(std::move(config))
{
}

bool ThermalCapture::run(
    const std::atomic<bool> &stop,
    const std::function<void(const ThermalFrameEvent &)> &on_frame,
    std::string *error)
{
    const int fd = open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        set_error(error, "open thermal " + config_.device);
        return false;
    }

    std::vector<MappedBuffer> buffers;
    bool streaming = false;
    auto cleanup = [&]() {
        if (streaming) {
            v4l2_buf_type type = V4L2_BUF_TYPE_META_CAPTURE;
            ioctl_retry(fd, VIDIOC_STREAMOFF, &type);
        }
        for (const MappedBuffer &buffer : buffers) {
            if (buffer.address != nullptr)
                munmap(buffer.address, buffer.length);
        }
        close(fd);
    };

    v4l2_capability capability{};
    if (ioctl_retry(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        set_error(error, "VIDIOC_QUERYCAP thermal");
        cleanup();
        return false;
    }
    const std::uint32_t required =
        V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
    if ((effective_capabilities(capability) & required) != required) {
        if (error != nullptr)
            *error = config_.device + " lacks META_CAPTURE/STREAMING";
        cleanup();
        return false;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_META_CAPTURE;
    if (ioctl_retry(fd, VIDIOC_G_FMT, &format) < 0) {
        set_error(error, "VIDIOC_G_FMT thermal");
        cleanup();
        return false;
    }
    if (format.fmt.meta.dataformat != V4L2_META_FMT_ZZH_MLX90640 ||
        format.fmt.meta.buffersize != ZZH_MLX90640_META_V1_SIZE) {
        if (error != nullptr)
            *error = "thermal node did not expose ZMLX v1";
        cleanup();
        return false;
    }
    stats_.buffer_size = format.fmt.meta.buffersize;

    v4l2_requestbuffers request{};
    request.count = config_.requested_buffers;
    request.type = V4L2_BUF_TYPE_META_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        set_error(error, "VIDIOC_REQBUFS thermal");
        cleanup();
        return false;
    }
    stats_.allocated_buffers = request.count;
    buffers.resize(request.count);

    for (std::uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_META_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (ioctl_retry(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            set_error(error, "VIDIOC_QUERYBUF thermal");
            cleanup();
            return false;
        }
        buffers[index].length = buffer.length;
        buffers[index].address = mmap(nullptr, buffer.length,
                                      PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                      buffer.m.offset);
        if (buffers[index].address == MAP_FAILED) {
            buffers[index].address = nullptr;
            set_error(error, "mmap thermal");
            cleanup();
            return false;
        }
        if (ioctl_retry(fd, VIDIOC_QBUF, &buffer) < 0) {
            set_error(error, "VIDIOC_QBUF thermal initial");
            cleanup();
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_META_CAPTURE;
    if (ioctl_retry(fd, VIDIOC_STREAMON, &type) < 0) {
        set_error(error, "VIDIOC_STREAMON thermal");
        cleanup();
        return false;
    }
    streaming = true;

    while (!stop.load()) {
        pollfd descriptor{fd, POLLIN | POLLPRI, 0};
        const int poll_result = poll(&descriptor, 1, config_.poll_timeout_ms);
        if (poll_result == 0) {
            ++stats_.poll_timeouts;
            continue;
        }
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            set_error(error, "poll thermal");
            cleanup();
            return false;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            if (error != nullptr)
                *error = "thermal poll reported error revents=" +
                         std::to_string(descriptor.revents);
            cleanup();
            return false;
        }

        v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_META_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl_retry(fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                continue;
            set_error(error, "VIDIOC_DQBUF thermal");
            cleanup();
            return false;
        }

        if (stats_.dqbuf_count == 0) {
            stats_.first_sequence = buffer.sequence;
        } else if (buffer.sequence != stats_.last_sequence + 1U) {
            ++stats_.sequence_gaps;
        }
        stats_.last_sequence = buffer.sequence;
        ++stats_.dqbuf_count;

        ThermalFrameEvent event;
        event.sequence = buffer.sequence;
        event.arrival_ns = monotonic_now_ns();
        const bool buffer_index_valid = buffer.index < buffers.size();
        const bool size_valid = buffer.bytesused >= sizeof(zzh_mlx90640_meta_v1);
        const bool payload_valid = buffer_index_valid && size_valid &&
            valid_meta(*static_cast<const zzh_mlx90640_meta_v1 *>(
                           buffers[buffer.index].address),
                       &event);
        if (!payload_valid) {
            ++stats_.invalid_payloads;
        } else {
            if (!has_monotonic_timestamp(buffer))
                ++stats_.missing_monotonic_timestamp;
            const std::uint64_t v4l2_timestamp = buffer_timestamp_ns(buffer);
            const std::uint64_t delta = v4l2_timestamp >= event.second_ready_ns
                ? v4l2_timestamp - event.second_ready_ns
                : event.second_ready_ns - v4l2_timestamp;
            if (delta > 1'000'000ULL)
                ++stats_.timestamp_mismatches;
            ++stats_.valid_pairs;
            on_frame(event);
        }

        if (ioctl_retry(fd, VIDIOC_QBUF, &buffer) < 0) {
            set_error(error, "VIDIOC_QBUF thermal recycle");
            cleanup();
            return false;
        }
    }

    cleanup();
    return true;
}

}  // namespace p2
