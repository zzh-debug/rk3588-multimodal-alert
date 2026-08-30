#include "p2/capture/visible_capture.hpp"

#include "p2/capture/v4l2_utils.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
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

class VisibleCaptureSession {
public:
    explicit VisibleCaptureSession(int fd)
        : fd_(fd)
    {
    }

    ~VisibleCaptureSession()
    {
        std::lock_guard<std::mutex> lock(ioctl_mutex_);
        if (streaming_) {
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ioctl_retry(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (const MappedPlane &buffer : buffers_) {
            if (buffer.address != nullptr)
                munmap(buffer.address, buffer.length);
        }
        if (fd_ >= 0)
            close(fd_);
    }

    VisibleCaptureSession(const VisibleCaptureSession &) = delete;
    VisibleCaptureSession &operator=(const VisibleCaptureSession &) = delete;

    int fd() const { return fd_; }
    std::vector<MappedPlane> &buffers() { return buffers_; }
    const std::vector<MappedPlane> &buffers() const { return buffers_; }

    int ioctl(unsigned long request, void *argument)
    {
        std::lock_guard<std::mutex> lock(ioctl_mutex_);
        return ioctl_retry(fd_, request, argument);
    }

    void set_streaming(bool streaming)
    {
        std::lock_guard<std::mutex> lock(ioctl_mutex_);
        streaming_ = streaming;
    }

    std::uint32_t mark_leased()
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++outstanding_leases_;
        lease_high_watermark_ = std::max(lease_high_watermark_,
                                         outstanding_leases_);
        return lease_high_watermark_;
    }

    void recycle(std::uint32_t index)
    {
        v4l2_buffer buffer{};
        v4l2_plane plane{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = 1;
        buffer.m.planes = &plane;

        int result = 0;
        int saved_errno = 0;
        {
            std::lock_guard<std::mutex> lock(ioctl_mutex_);
            if (streaming_) {
                result = ioctl_retry(fd_, VIDIOC_QBUF, &buffer);
                saved_errno = errno;
            }
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (outstanding_leases_ > 0)
            --outstanding_leases_;
        if (result < 0 && recycle_error_.empty()) {
            recycle_error_ = "VIDIOC_QBUF visible lease recycle: ";
            recycle_error_ += std::strerror(saved_errno);
        }
    }

    bool get_recycle_error(std::string *error) const
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (recycle_error_.empty())
            return false;
        if (error != nullptr)
            *error = recycle_error_;
        return true;
    }

private:
    int fd_ = -1;
    std::vector<MappedPlane> buffers_;
    bool streaming_ = false;
    mutable std::mutex ioctl_mutex_;
    mutable std::mutex state_mutex_;
    std::uint32_t outstanding_leases_ = 0;
    std::uint32_t lease_high_watermark_ = 0;
    std::string recycle_error_;
};

VisibleFrameLease::VisibleFrameLease(
    VisibleFrameView view,
    std::uint32_t buffer_index,
    std::shared_ptr<VisibleCaptureSession> session)
    : view_(view),
      buffer_index_(buffer_index),
      session_(std::move(session))
{
}

VisibleFrameLease::~VisibleFrameLease()
{
    reset();
}

VisibleFrameLease::VisibleFrameLease(VisibleFrameLease &&other) noexcept
    : view_(other.view_),
      buffer_index_(other.buffer_index_),
      session_(std::move(other.session_))
{
    other.view_ = {};
    other.buffer_index_ = 0;
}

VisibleFrameLease &VisibleFrameLease::operator=(
    VisibleFrameLease &&other) noexcept
{
    if (this == &other)
        return *this;
    reset();
    view_ = other.view_;
    buffer_index_ = other.buffer_index_;
    session_ = std::move(other.session_);
    other.view_ = {};
    other.buffer_index_ = 0;
    return *this;
}

void VisibleFrameLease::reset()
{
    if (session_ == nullptr)
        return;
    std::shared_ptr<VisibleCaptureSession> session = std::move(session_);
    session->recycle(buffer_index_);
    view_ = {};
    buffer_index_ = 0;
}

VisibleCapture::VisibleCapture(VisibleCaptureConfig config)
    : config_(std::move(config))
{
}

bool VisibleCapture::run(
    const std::atomic<bool> &stop,
    const std::function<void(const VisibleFrameEvent &)> &on_frame,
    std::string *error)
{
    return run_frames(
        stop,
        [&on_frame](const VisibleFrameView &frame) {
            on_frame(frame.event);
        },
        error);
}

bool VisibleCapture::run_frames(
    const std::atomic<bool> &stop,
    const std::function<void(const VisibleFrameView &)> &on_frame,
    std::string *error)
{
    return run_leased_frames(
        stop,
        [&on_frame](VisibleFrameLease &&lease) {
            on_frame(lease.view());
        },
        error);
}

bool VisibleCapture::run_leased_frames(
    const std::atomic<bool> &stop,
    const std::function<void(VisibleFrameLease &&)> &on_frame,
    std::string *error)
{
    stats_ = {};
    const int fd = open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        set_error(error, "open visible " + config_.device);
        return false;
    }
    auto session = std::make_shared<VisibleCaptureSession>(fd);

    v4l2_capability capability{};
    if (session->ioctl(VIDIOC_QUERYCAP, &capability) < 0) {
        set_error(error, "VIDIOC_QUERYCAP visible");
        return false;
    }
    const std::uint32_t required =
        V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
    if ((effective_capabilities(capability) & required) != required) {
        if (error != nullptr)
            *error = config_.device + " lacks MPLANE/STREAMING";
        return false;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = config_.width;
    format.fmt.pix_mp.height = config_.height;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    format.fmt.pix_mp.num_planes = 1;
    if (session->ioctl(VIDIOC_S_FMT, &format) < 0) {
        set_error(error, "VIDIOC_S_FMT visible");
        return false;
    }
    if (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
        format.fmt.pix_mp.num_planes != 1) {
        if (error != nullptr)
            *error = "visible node did not negotiate one-plane NV12";
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
    if (session->ioctl(VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        set_error(error, "VIDIOC_REQBUFS visible");
        return false;
    }
    stats_.allocated_buffers = request.count;
    session->buffers().resize(request.count);

    for (std::uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer{};
        v4l2_plane plane{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (session->ioctl(VIDIOC_QUERYBUF, &buffer) < 0) {
            set_error(error, "VIDIOC_QUERYBUF visible");
            return false;
        }
        MappedPlane &mapped = session->buffers()[index];
        mapped.length = plane.length;
        mapped.address = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, session->fd(), plane.m.mem_offset);
        if (mapped.address == MAP_FAILED) {
            mapped.address = nullptr;
            set_error(error, "mmap visible");
            return false;
        }
        if (session->ioctl(VIDIOC_QBUF, &buffer) < 0) {
            set_error(error, "VIDIOC_QBUF visible initial");
            return false;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (session->ioctl(VIDIOC_STREAMON, &type) < 0) {
        set_error(error, "VIDIOC_STREAMON visible");
        return false;
    }
    session->set_streaming(true);

    while (!stop.load()) {
        if (session->get_recycle_error(error))
            return false;
        pollfd descriptor{session->fd(), POLLIN | POLLPRI, 0};
        const int poll_result = poll(&descriptor, 1, config_.poll_timeout_ms);
        if (poll_result == 0) {
            ++stats_.poll_timeouts;
            continue;
        }
        if (poll_result < 0) {
            if (errno == EINTR)
                continue;
            set_error(error, "poll visible");
            return false;
        }
        if ((descriptor.revents & (POLLHUP | POLLNVAL)) != 0) {
            if (error != nullptr)
                *error = "visible poll reported error revents=" +
                         std::to_string(descriptor.revents);
            return false;
        }

        v4l2_buffer buffer{};
        v4l2_plane plane{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = 1;
        buffer.m.planes = &plane;
        if (session->ioctl(VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN)
                continue;
            set_error(error, "VIDIOC_DQBUF visible");
            return false;
        }
        const std::vector<MappedPlane> &buffers = session->buffers();
        if (buffer.index >= buffers.size() ||
            plane.data_offset > plane.bytesused ||
            plane.data_offset > buffers[buffer.index].length) {
            if (error != nullptr)
                *error = "visible driver returned an invalid buffer index/offset";
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

        const MappedPlane &mapped = buffers[buffer.index];
        const std::size_t payload_size = std::min<std::size_t>(
            static_cast<std::size_t>(plane.bytesused - plane.data_offset),
            mapped.length - plane.data_offset);
        VisibleFrameView frame;
        frame.event = event;
        frame.data = static_cast<const std::uint8_t *>(mapped.address) +
                     plane.data_offset;
        frame.size = payload_size;
        frame.width = stats_.width;
        frame.height = stats_.height;
        frame.bytes_per_line = stats_.bytes_per_line;
        stats_.lease_high_watermark = session->mark_leased();
        VisibleFrameLease lease(frame, buffer.index, session);
        on_frame(std::move(lease));
    }

    return !session->get_recycle_error(error);
}

}  // namespace p2
