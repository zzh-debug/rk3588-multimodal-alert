#include "p2/capture/device_discovery.hpp"

#include "p2/capture/v4l2_utils.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/videodev2.h>
#include <linux/zzh_mlx90640_meta.h>

namespace p2 {
namespace {

constexpr unsigned int kMaxMediaNodes = 64;
constexpr unsigned int kMaxVideoNodes = 256;

std::string numbered_device(const char *prefix, unsigned int index)
{
    char path[64];
    std::snprintf(path, sizeof(path), "%s%u", prefix, index);
    return path;
}

std::string find_video_by_device_number(unsigned int wanted_major,
                                        unsigned int wanted_minor)
{
    for (unsigned int index = 0; index < kMaxVideoNodes; ++index) {
        const std::string path = numbered_device("/dev/video", index);
        struct stat status {};
        if (stat(path.c_str(), &status) == 0 && S_ISCHR(status.st_mode) &&
            major(status.st_rdev) == wanted_major &&
            minor(status.st_rdev) == wanted_minor)
            return path;
    }
    return {};
}

bool contains(const char *text, const char *needle)
{
    return std::strstr(text, needle) != nullptr;
}

}  // namespace

bool discover_visible_device(std::string *video_path, std::string *media_path,
                             std::string *error)
{
    if (video_path == nullptr || media_path == nullptr || error == nullptr)
        return false;

    for (unsigned int index = 0; index < kMaxMediaNodes; ++index) {
        const std::string media = numbered_device("/dev/media", index);
        const int fd = open(media.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        bool has_target_bridge = false;
        bool has_target_sensor = false;
        unsigned int mainpath_major = 0;
        unsigned int mainpath_minor = 0;
        media_entity_desc entity{};
        entity.id = MEDIA_ENT_ID_FLAG_NEXT;
        while (ioctl_retry(fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0) {
            if (contains(entity.name, "rkcif-mipi-lvds2"))
                has_target_bridge = true;
            if (contains(entity.name, "imx415"))
                has_target_sensor = true;
            if (std::strcmp(entity.name, "rkisp_mainpath") == 0) {
                mainpath_major = entity.dev.major;
                mainpath_minor = entity.dev.minor;
            }
            entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
        }
        close(fd);

        if (mainpath_major == 0 ||
            (!has_target_bridge && !has_target_sensor))
            continue;
        const std::string video = find_video_by_device_number(
            mainpath_major, mainpath_minor);
        if (!video.empty()) {
            *video_path = video;
            *media_path = media;
            return true;
        }
    }

    *error = "no rkisp_mainpath connected to IMX415/rkcif-mipi-lvds2";
    return false;
}

bool discover_thermal_device(std::string *video_path, std::string *error)
{
    if (video_path == nullptr || error == nullptr)
        return false;

    for (unsigned int index = 0; index < kMaxVideoNodes; ++index) {
        const std::string path = numbered_device("/dev/video", index);
        const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;

        v4l2_capability capability{};
        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_META_CAPTURE;
        const bool match =
            ioctl_retry(fd, VIDIOC_QUERYCAP, &capability) == 0 &&
            std::strcmp(reinterpret_cast<const char *>(capability.driver),
                        "zzh_mlx90640") == 0 &&
            (effective_capabilities(capability) &
             (V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING)) ==
                (V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING) &&
            ioctl_retry(fd, VIDIOC_G_FMT, &format) == 0 &&
            format.fmt.meta.dataformat == V4L2_META_FMT_ZZH_MLX90640 &&
            format.fmt.meta.buffersize == ZZH_MLX90640_META_V1_SIZE;
        close(fd);
        if (match) {
            *video_path = path;
            return true;
        }
    }

    *error = "no zzh_mlx90640 ZMLX Meta Capture node";
    return false;
}

bool discover_devices(DiscoveredDevices *devices, std::string *error)
{
    if (devices == nullptr || error == nullptr)
        return false;
    if (!discover_visible_device(&devices->visible, &devices->visible_media,
                                 error))
        return false;
    if (!discover_thermal_device(&devices->thermal, error))
        return false;
    return true;
}

}  // namespace p2
