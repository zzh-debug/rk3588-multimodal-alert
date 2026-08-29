#include "p2/evaluation/person_dataset.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <vector>

namespace p2 {
namespace {

float clamp_float(float value, float low, float high)
{
    return std::max(low, std::min(value, high));
}

std::uint8_t clamp_byte(int value)
{
    return static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
}

void write_u16(std::ostream &output, std::uint16_t value)
{
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ostream &output, std::uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool valid_geometry(const PersonPreviewGeometry &geometry)
{
    return geometry.source_width != 0 && geometry.source_height != 0 &&
        geometry.oriented_width != 0 && geometry.oriented_height != 0 &&
        geometry.preview_width != 0 && geometry.preview_height != 0;
}

void oriented_to_source_pixel(const PersonPreviewGeometry &geometry,
                              std::uint32_t oriented_x,
                              std::uint32_t oriented_y,
                              std::uint32_t *source_x,
                              std::uint32_t *source_y)
{
    switch (geometry.rotation) {
    case ImageRotation::kNone:
        *source_x = oriented_x;
        *source_y = oriented_y;
        return;
    case ImageRotation::kClockwise90:
        *source_x = oriented_y;
        *source_y = geometry.source_height - 1U - oriented_x;
        return;
    case ImageRotation::kClockwise180:
        *source_x = geometry.source_width - 1U - oriented_x;
        *source_y = geometry.source_height - 1U - oriented_y;
        return;
    case ImageRotation::kClockwise270:
        *source_x = geometry.source_width - 1U - oriented_y;
        *source_y = oriented_x;
        return;
    }
    *source_x = 0;
    *source_y = 0;
}

FloatBox source_to_oriented_box(const PersonPreviewGeometry &geometry,
                                const FloatBox &source)
{
    const float width = static_cast<float>(geometry.source_width);
    const float height = static_cast<float>(geometry.source_height);
    switch (geometry.rotation) {
    case ImageRotation::kNone:
        return source;
    case ImageRotation::kClockwise90:
        return {height - source.bottom, source.left,
                height - source.top, source.right};
    case ImageRotation::kClockwise180:
        return {width - source.right, height - source.bottom,
                width - source.left, height - source.top};
    case ImageRotation::kClockwise270:
        return {source.top, width - source.right,
                source.bottom, width - source.left};
    }
    return {};
}

}  // namespace

bool make_person_preview_geometry(std::uint32_t source_width,
                                  std::uint32_t source_height,
                                  std::uint32_t preview_long_edge,
                                  ImageRotation rotation,
                                  PersonPreviewGeometry *geometry,
                                  std::string *error)
{
    if (geometry == nullptr || source_width == 0 || source_height == 0 ||
        preview_long_edge == 0) {
        if (error != nullptr)
            *error = "preview dimensions and output must be non-zero";
        return false;
    }
    const bool swaps_axes = rotation == ImageRotation::kClockwise90 ||
        rotation == ImageRotation::kClockwise270;
    PersonPreviewGeometry result;
    result.source_width = source_width;
    result.source_height = source_height;
    result.oriented_width = swaps_axes ? source_height : source_width;
    result.oriented_height = swaps_axes ? source_width : source_height;
    result.rotation = rotation;

    const double scale = static_cast<double>(preview_long_edge) /
        static_cast<double>(std::max(result.oriented_width,
                                     result.oriented_height));
    result.preview_width = std::max(1U, static_cast<std::uint32_t>(
        std::lround(static_cast<double>(result.oriented_width) * scale)));
    result.preview_height = std::max(1U, static_cast<std::uint32_t>(
        std::lround(static_cast<double>(result.oriented_height) * scale)));
    *geometry = result;
    return true;
}

FloatBox source_to_person_preview_box(
    const PersonPreviewGeometry &geometry,
    const FloatBox &source_box)
{
    if (!valid_geometry(geometry))
        return {};
    FloatBox source = source_box;
    const float source_width = static_cast<float>(geometry.source_width);
    const float source_height = static_cast<float>(geometry.source_height);
    source.left = clamp_float(source.left, 0.0F, source_width);
    source.right = clamp_float(source.right, 0.0F, source_width);
    source.top = clamp_float(source.top, 0.0F, source_height);
    source.bottom = clamp_float(source.bottom, 0.0F, source_height);
    if (source.left > source.right)
        std::swap(source.left, source.right);
    if (source.top > source.bottom)
        std::swap(source.top, source.bottom);

    FloatBox oriented = source_to_oriented_box(geometry, source);
    const float scale_x = static_cast<float>(geometry.preview_width) /
        static_cast<float>(geometry.oriented_width);
    const float scale_y = static_cast<float>(geometry.preview_height) /
        static_cast<float>(geometry.oriented_height);
    oriented.left *= scale_x;
    oriented.right *= scale_x;
    oriented.top *= scale_y;
    oriented.bottom *= scale_y;
    return oriented;
}

bool write_nv12_person_preview_bmp(
    const std::string &path,
    const std::uint8_t *nv12,
    std::size_t size,
    std::uint32_t bytes_per_line,
    const PersonPreviewGeometry &geometry,
    std::string *error)
{
    if (nv12 == nullptr || !valid_geometry(geometry) ||
        bytes_per_line < geometry.source_width) {
        if (error != nullptr)
            *error = "invalid NV12 preview input";
        return false;
    }
    const std::uint64_t required = static_cast<std::uint64_t>(bytes_per_line) *
        geometry.source_height * 3U / 2U;
    if (required > size) {
        if (error != nullptr)
            *error = "NV12 buffer is smaller than stride-based frame size";
        return false;
    }

    const std::uint64_t raw_row_bytes =
        static_cast<std::uint64_t>(geometry.preview_width) * 3U;
    const std::uint64_t row_bytes = (raw_row_bytes + 3U) & ~3ULL;
    const std::uint64_t pixel_bytes = row_bytes * geometry.preview_height;
    constexpr std::uint32_t pixel_offset = 54U;
    const std::uint64_t file_bytes = pixel_offset + pixel_bytes;
    if (row_bytes > std::numeric_limits<std::uint32_t>::max() ||
        file_bytes > std::numeric_limits<std::uint32_t>::max()) {
        if (error != nullptr)
            *error = "preview BMP is too large";
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error != nullptr)
            *error = "cannot open preview BMP: " + path;
        return false;
    }
    output.put('B');
    output.put('M');
    write_u32(output, static_cast<std::uint32_t>(file_bytes));
    write_u16(output, 0U);
    write_u16(output, 0U);
    write_u32(output, pixel_offset);
    write_u32(output, 40U);
    write_u32(output, geometry.preview_width);
    write_u32(output, geometry.preview_height);
    write_u16(output, 1U);
    write_u16(output, 24U);
    write_u32(output, 0U);
    write_u32(output, static_cast<std::uint32_t>(pixel_bytes));
    write_u32(output, 2835U);
    write_u32(output, 2835U);
    write_u32(output, 0U);
    write_u32(output, 0U);

    const std::size_t row_size = static_cast<std::size_t>(row_bytes);
    std::vector<std::uint8_t> row(row_size, 0U);
    const std::size_t uv_offset = static_cast<std::size_t>(bytes_per_line) *
        geometry.source_height;
    for (std::uint32_t file_y = 0; file_y < geometry.preview_height;
         ++file_y) {
        const std::uint32_t preview_y =
            geometry.preview_height - 1U - file_y;
        const std::uint32_t oriented_y = std::min(
            geometry.oriented_height - 1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(preview_y) *
                 geometry.oriented_height) / geometry.preview_height));
        for (std::uint32_t preview_x = 0;
             preview_x < geometry.preview_width; ++preview_x) {
            const std::uint32_t oriented_x = std::min(
                geometry.oriented_width - 1U,
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(preview_x) *
                     geometry.oriented_width) / geometry.preview_width));
            std::uint32_t source_x = 0;
            std::uint32_t source_y = 0;
            oriented_to_source_pixel(geometry, oriented_x, oriented_y,
                                     &source_x, &source_y);
            const int y_value = nv12[
                static_cast<std::size_t>(source_y) * bytes_per_line +
                source_x];
            const std::size_t uv_index = uv_offset +
                static_cast<std::size_t>(source_y / 2U) * bytes_per_line +
                (source_x & ~1U);
            const int u_value = nv12[uv_index];
            const int v_value = nv12[uv_index + 1U];
            const int c = std::max(0, y_value - 16);
            const int d = u_value - 128;
            const int e = v_value - 128;
            const std::uint8_t red = clamp_byte(
                (298 * c + 409 * e + 128) >> 8);
            const std::uint8_t green = clamp_byte(
                (298 * c - 100 * d - 208 * e + 128) >> 8);
            const std::uint8_t blue = clamp_byte(
                (298 * c + 516 * d + 128) >> 8);
            const std::size_t destination =
                static_cast<std::size_t>(preview_x) * 3U;
            row[destination] = blue;
            row[destination + 1U] = green;
            row[destination + 2U] = red;
        }
        output.write(reinterpret_cast<const char *>(row.data()),
                     static_cast<std::streamsize>(row.size()));
        if (!output) {
            if (error != nullptr)
                *error = "failed while writing preview BMP: " + path;
            return false;
        }
    }
    return true;
}

}  // namespace p2
