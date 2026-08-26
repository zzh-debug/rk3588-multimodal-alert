#pragma once

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <string>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

namespace p2 {

inline int ioctl_retry(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

inline std::uint32_t effective_capabilities(
    const v4l2_capability &capability)
{
    if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0)
        return capability.device_caps;
    return capability.capabilities;
}

inline std::uint64_t monotonic_now_ns()
{
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

inline std::uint64_t buffer_timestamp_ns(const v4l2_buffer &buffer)
{
    return static_cast<std::uint64_t>(buffer.timestamp.tv_sec) *
               1'000'000'000ULL +
           static_cast<std::uint64_t>(buffer.timestamp.tv_usec) * 1'000ULL;
}

inline bool has_monotonic_timestamp(const v4l2_buffer &buffer)
{
    return (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) ==
           V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
}

inline std::string errno_message(const std::string &operation)
{
    return operation + ": errno=" + std::to_string(errno);
}

}  // namespace p2
