#include "p2/illumination/illumination_hardware.hpp"

#include <linux/videodev2.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace p2 {
namespace {

bool ioctl_retry(int fd, unsigned long request, void *argument)
{
    int status = 0;
    do {
        status = ioctl(fd, request, argument);
    } while (status < 0 && errno == EINTR);
    return status >= 0;
}

void set_errno_error(std::string *error, const std::string &operation)
{
    if (error != nullptr)
        *error = operation + ": " + std::strerror(errno);
}

bool read_text(const std::filesystem::path &path, std::string *text)
{
    std::ifstream input(path);
    if (!input)
        return false;
    std::getline(input, *text);
    return input.good() || input.eof();
}

}  // namespace

SysfsPwmLed::SysfsPwmLed(std::string path)
    : path_(std::move(path))
{
}

SysfsPwmLed::~SysfsPwmLed()
{
    if (initialized_)
        off(nullptr);
}

bool SysfsPwmLed::read_value(const std::string &name,
                             std::uint32_t *value,
                             std::string *error) const
{
    std::ifstream input(path_ + "/" + name);
    unsigned long parsed = 0;
    if (!input || !(input >> parsed) ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        if (error != nullptr)
            *error = "cannot read LED " + name + " from " + path_;
        return false;
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool SysfsPwmLed::write_brightness(std::uint32_t brightness,
                                   std::string *error)
{
    const std::string value = std::to_string(brightness);
    const std::string file = path_ + "/brightness";
    const int fd = open(file.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        ++stats_.failures;
        set_errno_error(error, "open LED brightness");
        return false;
    }
    const ssize_t written = write(fd, value.data(), value.size());
    const int saved_errno = errno;
    close(fd);
    if (written != static_cast<ssize_t>(value.size())) {
        ++stats_.failures;
        errno = saved_errno;
        set_errno_error(error, "write LED brightness");
        return false;
    }
    std::uint32_t readback = 0;
    if (!read_value("brightness", &readback, error) ||
        readback != brightness) {
        ++stats_.failures;
        if (error != nullptr && readback != brightness)
            *error = "LED brightness readback mismatch";
        return false;
    }
    ++stats_.writes;
    stats_.maximum_commanded = std::max(stats_.maximum_commanded,
                                        brightness);
    stats_.final_brightness = brightness;
    return true;
}

bool SysfsPwmLed::initialize(std::uint32_t safety_limit,
                             std::string *error)
{
    if (safety_limit == 0U ||
        !read_value("max_brightness", &maximum_brightness_, error) ||
        safety_limit > maximum_brightness_) {
        if (error != nullptr && maximum_brightness_ != 0U &&
            safety_limit > maximum_brightness_)
            *error = "LED safety limit exceeds max_brightness";
        return false;
    }
    safety_limit_ = safety_limit;
    initialized_ = true;
    if (!write_brightness(0U, error)) {
        initialized_ = false;
        return false;
    }
    return true;
}

bool SysfsPwmLed::set_brightness(std::uint32_t brightness,
                                 std::string *error)
{
    if (!initialized_ || brightness > safety_limit_) {
        ++stats_.failures;
        if (error != nullptr)
            *error = "LED is uninitialized or command exceeds safety limit";
        return false;
    }
    if (brightness == stats_.final_brightness)
        return true;
    return write_brightness(brightness, error);
}

bool SysfsPwmLed::off(std::string *error)
{
    if (!initialized_)
        return true;
    return set_brightness(0U, error);
}

struct Imx415ExposureMonitor::Impl {
    ~Impl()
    {
        if (fd >= 0)
            close(fd);
    }

    int fd = -1;
    v4l2_queryctrl exposure_query{};
    v4l2_queryctrl gain_query{};
};

Imx415ExposureMonitor::Imx415ExposureMonitor()
    : impl_(new Impl)
{
}

Imx415ExposureMonitor::~Imx415ExposureMonitor() = default;

bool Imx415ExposureMonitor::initialize(
    const std::string &device_override,
    std::string *error)
{
    if (impl_->fd >= 0)
        return true;
    device_ = device_override;
    if (device_.empty()) {
        const std::filesystem::path root("/sys/class/video4linux");
        std::error_code iterator_error;
        for (const auto &entry :
             std::filesystem::directory_iterator(root, iterator_error)) {
            const std::string basename = entry.path().filename().string();
            if (basename.rfind("v4l-subdev", 0) != 0)
                continue;
            std::string name;
            if (!read_text(entry.path() / "name", &name))
                continue;
            if (name.find("zzh_imx415") != std::string::npos ||
                name.find("imx415") != std::string::npos) {
                device_ = "/dev/" + basename;
                break;
            }
        }
        if (iterator_error) {
            if (error != nullptr)
                *error = "cannot enumerate video4linux subdevices: " +
                    iterator_error.message();
            return false;
        }
    }
    if (device_.empty()) {
        if (error != nullptr)
            *error = "cannot discover IMX415 V4L2 subdevice";
        return false;
    }
    impl_->fd = open(device_.c_str(), O_RDONLY | O_CLOEXEC);
    if (impl_->fd < 0) {
        set_errno_error(error, "open IMX415 subdevice " + device_);
        return false;
    }
    impl_->exposure_query.id = V4L2_CID_EXPOSURE;
    impl_->gain_query.id = V4L2_CID_ANALOGUE_GAIN;
    if (!ioctl_retry(impl_->fd, VIDIOC_QUERYCTRL,
                     &impl_->exposure_query) ||
        !ioctl_retry(impl_->fd, VIDIOC_QUERYCTRL,
                     &impl_->gain_query) ||
        (impl_->exposure_query.flags & V4L2_CTRL_FLAG_DISABLED) != 0U ||
        (impl_->gain_query.flags & V4L2_CTRL_FLAG_DISABLED) != 0U) {
        set_errno_error(error, "query IMX415 exposure controls");
        close(impl_->fd);
        impl_->fd = -1;
        return false;
    }
    return true;
}

bool Imx415ExposureMonitor::sample(ExposureTelemetry *telemetry,
                                   std::string *error)
{
    if (impl_->fd < 0 || telemetry == nullptr) {
        if (error != nullptr)
            *error = "IMX415 exposure monitor is uninitialized";
        return false;
    }
    v4l2_control exposure{};
    v4l2_control gain{};
    exposure.id = V4L2_CID_EXPOSURE;
    gain.id = V4L2_CID_ANALOGUE_GAIN;
    if (!ioctl_retry(impl_->fd, VIDIOC_G_CTRL, &exposure) ||
        !ioctl_retry(impl_->fd, VIDIOC_G_CTRL, &gain)) {
        set_errno_error(error, "read IMX415 exposure controls");
        return false;
    }
    const auto ratio = [](std::int32_t value, std::int32_t minimum,
                          std::int32_t maximum) {
        if (maximum <= minimum)
            return 0.0F;
        return std::clamp(
            static_cast<float>(value - minimum) /
                static_cast<float>(maximum - minimum),
            0.0F, 1.0F);
    };
    ExposureTelemetry result;
    result.valid = true;
    result.exposure = exposure.value;
    result.exposure_min = impl_->exposure_query.minimum;
    result.exposure_max = impl_->exposure_query.maximum;
    result.analogue_gain = gain.value;
    result.analogue_gain_min = impl_->gain_query.minimum;
    result.analogue_gain_max = impl_->gain_query.maximum;
    result.exposure_ratio = ratio(
        result.exposure, result.exposure_min, result.exposure_max);
    result.analogue_gain_ratio = ratio(
        result.analogue_gain, result.analogue_gain_min,
        result.analogue_gain_max);
    *telemetry = result;
    return true;
}

}  // namespace p2
