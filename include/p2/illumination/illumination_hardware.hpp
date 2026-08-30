#pragma once

#include "p2/illumination/illumination_controller.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace p2 {

struct PwmLedStats {
    std::uint64_t writes = 0;
    std::uint64_t failures = 0;
    std::uint32_t maximum_commanded = 0;
    std::uint32_t final_brightness = 0;
};

class SysfsPwmLed {
public:
    explicit SysfsPwmLed(
        std::string path = "/sys/class/leds/zzh:white:fill");
    ~SysfsPwmLed();

    SysfsPwmLed(const SysfsPwmLed &) = delete;
    SysfsPwmLed &operator=(const SysfsPwmLed &) = delete;

    bool initialize(std::uint32_t safety_limit, std::string *error);
    bool set_brightness(std::uint32_t brightness, std::string *error);
    bool off(std::string *error);
    std::uint32_t maximum_brightness() const { return maximum_brightness_; }
    const PwmLedStats &stats() const { return stats_; }

private:
    bool read_value(const std::string &name, std::uint32_t *value,
                    std::string *error) const;
    bool write_brightness(std::uint32_t brightness, std::string *error);

    std::string path_;
    std::uint32_t maximum_brightness_ = 0;
    std::uint32_t safety_limit_ = 0;
    bool initialized_ = false;
    PwmLedStats stats_;
};

class Imx415ExposureMonitor {
public:
    Imx415ExposureMonitor();
    ~Imx415ExposureMonitor();

    Imx415ExposureMonitor(const Imx415ExposureMonitor &) = delete;
    Imx415ExposureMonitor &operator=(const Imx415ExposureMonitor &) = delete;

    bool initialize(const std::string &device_override,
                    std::string *error);
    bool sample(ExposureTelemetry *telemetry, std::string *error);
    const std::string &device() const { return device_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string device_;
};

}  // namespace p2
