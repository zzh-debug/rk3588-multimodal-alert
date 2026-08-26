#include "p2/capture/visible_capture.hpp"

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

namespace p2 {
namespace {

struct MappedPlane {
    void *address = nullptr;
    std::size_t length = 0;
};

void set_error(std::string *error, const std::string &operation)
{
    if (error != nullptr)
        *error = operation + ": " + std::strerror(errno);
}

}  // namespace

VisibleCapture::VisibleCapture(VisibleCaptureConfig config)
    : config_(std::move(config))
{
}

bool VisibleCapture::run(
    const std::atomic<bool> &stop,
    const std::function<void(const VisibleFrameEvent &)> &on_frame,
    std::string *error)
{
    const int fd = open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        set_error(error, "open visible " + config_.device);
        return false;
    }

    std::vector<MappedPlane> buffers;
    bool streaming = false;
    auto cleanup = [&]() {
        if (streaming) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ioctl_retry(fd, VIDIOC_STREAMOFF, &type);
        }
        for (const MappedPlane &buffer : buffers) {
            if (buffer.address != nullptr)
                munmap(buffer.address, buffer.length);
        }
        close(fd);
    };

    v4l2_capability capability{};
    if (ioctl_retry(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        set_error(error, "VIDIOC_QUERYCAP visible");
        cleanup();
        return false;
    }
    const std::uint32_t required =
        V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
    if ((effective_capabilities(capability) & required) != required) {
        if (error != nullptr)
            *error = config_.device + " lacks MPLANE/STREAMING";
        cleanup();
        return false;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = config_.width;
    format.fmt.pix_mp.height = config_.height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;
    if (ioctl_retry(fd, VIDIOC_S_FMT, &format) < 0) {
        set_error(error, "VIDIOC_S_FMT visible");
        cleanup();
        return false;
    }
    if (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
        format.fmt.pix_mp.num_planes != 1) {
        if (error != nullptr)
            *error = "visible node did not negotiate one-plane NV12";
        cleanup();
        return false;
    }
    stats_.width = format.fmt.pix_mp.width;
    stats_.height = format.fmt.pix_mp.height;
    stats_.bytes_per_line = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    stats_.size_image = format.fmt.pix_mp.plane_fmt[0].sizeimage;

    v4l2_requestbuffers request{};
    request.count = config_.requested_buffers;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        set_error(error, "VIDIOC_REQBUFS visible");
        cleanup();
        return false;
    }
    stats_.allocated_buffers = request.count;
    buffers.resize(request.count);

    for (std::uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer{};
        v4l2_plane plane{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (ioctl_retry(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            set_error(error, "VIDIOC_QUERYBUF visible");
            cleanup();
            return false;
        }
        buffers[index].length = plane.length;
        buffers[index].address = mmap(nullptr, plane.length,
                                      PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                                      plane.m.mem_offset);
        if (buffers[index].address == MAP_FAILED) {
            buffers[index].address = nullptr;
            set_error(error, "mmap visible");
            cleanup();
            return false;
        }
        if (ioctl_retry(fd, VIDIOC_QBUF, &buffer) < 0) {
            set_error(error, "VIDIOC_QBUF visible initial");
            cleanup();
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl_retry(fd, VIDIOC_STREAMON, &type) < 0) {
        set_error(error, "VIDIOC_STREAMON visible");
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
            set_error(error, "poll visible");
            cleanup();
            return false;
        }
        if ((descriptor.revents & (POLLHUP | POLLNVAL)) != 0) {
            if (error != nullptr)
                *error = "visible poll reported error revents=" +
                         std::to_string(descriptor.revents);
            cleanup();
            return false;
        }

        v4l2_buffer buffer{};
        v4l2_plane plane{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (ioctl_retry(fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                continue;
            set_error(error, "VIDIOC_DQBUF visible");
            cleanup();
            return false;
        }

        VisibleFrameEvent event;
        event.sequence = buffer.sequence;
        event.timestamp_ns = buffer_timestamp_ns(buffer);
        event.arrival_ns = monotonic_now_ns();
        event.bytes_used = plane.bytesused;
        event.flags = buffer.flags;
        if (stats_.dqbuf_count == 0) {
            stats_.first_sequence = buffer.sequence;
        } else if (buffer.sequence != stats_.last_sequence + 1U) {
            ++stats_.sequence_gaps;
        }
        stats_.last_sequence = buffer.sequence;
        ++stats_.dqbuf_count;
        if (plane.bytesused != stats_.size_image)
            ++stats_.bad_bytes_used;
        if (!has_monotonic_timestamp(buffer))
            ++stats_.missing_monotonic_timestamp;
        on_frame(event);

        if (ioctl_retry(fd, VIDIOC_QBUF, &buffer) < 0) {
            set_error(error, "VIDIOC_QBUF visible recycle");
            cleanup();
            return false;
        }
    }

    cleanup();
    return true;
}

}  // namespace p2
