#pragma once

#include "p2/inference/letterbox.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace p2 {

struct PersonPreviewGeometry {
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t oriented_width = 0;
    std::uint32_t oriented_height = 0;
    std::uint32_t preview_width = 0;
    std::uint32_t preview_height = 0;
    ImageRotation rotation = ImageRotation::kNone;
};

bool make_person_preview_geometry(std::uint32_t source_width,
                                  std::uint32_t source_height,
                                  std::uint32_t preview_long_edge,
                                  ImageRotation rotation,
                                  PersonPreviewGeometry *geometry,
                                  std::string *error);

FloatBox source_to_person_preview_box(
    const PersonPreviewGeometry &geometry,
    const FloatBox &source_box);

bool write_nv12_person_preview_bmp(
    const std::string &path,
    const std::uint8_t *nv12,
    std::size_t size,
    std::uint32_t bytes_per_line,
    const PersonPreviewGeometry &geometry,
    std::string *error);

}  // namespace p2
