#pragma once

#include <string>

namespace p2 {

struct DiscoveredDevices {
    std::string visible;
    std::string thermal;
    std::string visible_media;
};

bool discover_devices(DiscoveredDevices *devices, std::string *error);
bool discover_visible_device(std::string *video_path, std::string *media_path,
                             std::string *error);
bool discover_thermal_device(std::string *video_path, std::string *error);

}  // namespace p2
