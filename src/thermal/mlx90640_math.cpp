#include "p2/thermal/mlx90640_math.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>

#include "MLX90640_API.h"

namespace p2 {
namespace {

constexpr std::uint32_t kZmlxMagic = 0x584c4d5aU;
constexpr std::uint16_t kZmlxVersion = 1;
constexpr std::uint16_t kZmlxHeaderBytes = 24;
constexpr std::uint32_t kZmlxChessFlag = 1U << 0;
constexpr std::uint32_t kZmlxConsecutiveFlag = 1U << 1;
constexpr std::size_t kSubpageBytes = 1688;
constexpr std::size_t kSubpagePixelsOffset = 24;
constexpr std::size_t kSubpageAuxOffset = 1560;
constexpr std::size_t kFrameWords = 834;

std::uint16_t read_le16(const std::uint8_t *bytes)
{
    return static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(bytes[1]) << 8;
}

std::uint32_t read_le32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(read_le16(bytes)) |
        static_cast<std::uint32_t>(read_le16(bytes + 2)) << 16;
}

std::uint64_t read_le64(const std::uint8_t *bytes)
{
    return static_cast<std::uint64_t>(read_le32(bytes)) |
        static_cast<std::uint64_t>(read_le32(bytes + 4)) << 32;
}

std::uint16_t read_be16(const std::uint8_t *bytes)
{
    return static_cast<std::uint16_t>(bytes[0]) << 8 |
        static_cast<std::uint16_t>(bytes[1]);
}

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr)
        *error = message;
}

std::size_t deviating_pixel_count(const std::uint16_t pixels[5])
{
    std::size_t count = 0;
    while (count < 5 && pixels[count] != 0xFFFFU)
        ++count;
    return count;
}

struct DecodedPair {
    std::array<std::array<std::uint16_t, kFrameWords>, 2> frame_data{};
    std::array<std::uint64_t, 2> ready_ns{};
    std::array<std::uint64_t, 2> read_done_ns{};
    std::array<std::uint16_t, 2> subpage_id{};
    std::uint32_t pair_sequence = 0;
};

bool decode_zmlx(const ThermalFramePayload &payload, DecodedPair *decoded,
                 std::string *error)
{
    const std::uint8_t *bytes = payload.zmlx_bytes.data();
    if (read_le32(bytes) != kZmlxMagic ||
        read_le16(bytes + 4) != kZmlxVersion ||
        read_le16(bytes + 6) != kZmlxHeaderBytes ||
        read_le32(bytes + 8) != kZmlxMetaV1Bytes) {
        set_error(error, "invalid ZMLX v1 header");
        return false;
    }

    const std::uint32_t flags = read_le32(bytes + 16);
    if ((flags & (kZmlxChessFlag | kZmlxConsecutiveFlag)) !=
        (kZmlxChessFlag | kZmlxConsecutiveFlag)) {
        set_error(error, "ZMLX pair is not consecutive Chess mode");
        return false;
    }
    decoded->pair_sequence = read_le32(bytes + 12);

    for (std::size_t page = 0; page < 2; ++page) {
        const std::uint8_t *subpage =
            bytes + kZmlxHeaderBytes + page * kSubpageBytes;
        decoded->ready_ns[page] = read_le64(subpage);
        decoded->read_done_ns[page] = read_le64(subpage + 8);
        const std::uint16_t control = read_le16(subpage + 18);
        const std::uint16_t subpage_id = read_le16(subpage + 20);
        if (subpage_id > 1) {
            set_error(error, "invalid ZMLX subpage id");
            return false;
        }
        decoded->subpage_id[page] = subpage_id;
        for (std::size_t pixel = 0; pixel < kMlx90640Pixels; ++pixel) {
            decoded->frame_data[page][pixel] = read_le16(
                subpage + kSubpagePixelsOffset + pixel * 2);
        }
        for (std::size_t word = 0; word < 64; ++word) {
            decoded->frame_data[page][768 + word] = read_le16(
                subpage + kSubpageAuxOffset + word * 2);
        }
        decoded->frame_data[page][832] = control;
        decoded->frame_data[page][833] = subpage_id;
    }

    if (decoded->subpage_id[0] == decoded->subpage_id[1] ||
        decoded->ready_ns[0] == 0 ||
        decoded->ready_ns[1] <= decoded->ready_ns[0] ||
        decoded->read_done_ns[0] < decoded->ready_ns[0] ||
        decoded->read_done_ns[1] < decoded->ready_ns[1]) {
        set_error(error, "invalid ZMLX subpage order or timestamps");
        return false;
    }
    return true;
}

}  // namespace

struct Mlx90640Math::Impl {
    paramsMLX90640 parameters{};
    bool initialized = false;
    std::size_t broken_pixel_count = 0;
    std::size_t outlier_pixel_count = 0;
};

Mlx90640Math::Mlx90640Math() : impl_(std::make_unique<Impl>()) {}

Mlx90640Math::~Mlx90640Math() = default;
Mlx90640Math::Mlx90640Math(Mlx90640Math &&) noexcept = default;
Mlx90640Math &Mlx90640Math::operator=(Mlx90640Math &&) noexcept = default;

bool Mlx90640Math::initialize_eeprom_be(const std::uint8_t *bytes,
                                        std::size_t size,
                                        std::string *error)
{
    if (error != nullptr)
        error->clear();
    impl_->initialized = false;
    if (bytes == nullptr || size != kMlx90640EepromBytes) {
        set_error(error, "EEPROM must be exactly 1664 big-endian bytes");
        return false;
    }

    std::array<std::uint16_t, 832> words{};
    for (std::size_t index = 0; index < words.size(); ++index)
        words[index] = read_be16(bytes + index * 2);

    impl_->parameters = {};
    const int status =
        MLX90640_ExtractParameters(words.data(), &impl_->parameters);
    if (status != 0) {
        std::ostringstream message;
        message << "MLX90640_ExtractParameters failed with " << status;
        set_error(error, message.str());
        return false;
    }
    impl_->broken_pixel_count =
        deviating_pixel_count(impl_->parameters.brokenPixels);
    impl_->outlier_pixel_count =
        deviating_pixel_count(impl_->parameters.outlierPixels);
    impl_->initialized = true;
    return true;
}

bool Mlx90640Math::calculate(const ThermalFramePayload &frame,
                             const ThermalMathConfig &config,
                             ThermalMathResult *result,
                             std::string *error) const
{
    if (error != nullptr)
        error->clear();
    if (!impl_->initialized) {
        set_error(error, "MLX90640 parameters are not initialized");
        return false;
    }
    if (result == nullptr) {
        set_error(error, "result pointer is null");
        return false;
    }
    if (!std::isfinite(config.emissivity) || config.emissivity <= 0.0F ||
        config.emissivity > 1.0F ||
        !std::isfinite(config.reflected_temperature_offset_c)) {
        set_error(error, "emissivity must be in (0, 1] and offset finite");
        return false;
    }

    DecodedPair decoded;
    if (!decode_zmlx(frame, &decoded, error))
        return false;

    ThermalMathResult output;
    output.temperature_c.fill(std::numeric_limits<float>::quiet_NaN());
    output.image.fill(std::numeric_limits<float>::quiet_NaN());
    output.pair_sequence = decoded.pair_sequence;
    output.subpage_id = decoded.subpage_id;
    output.subpage_span_ns = decoded.ready_ns[1] - decoded.ready_ns[0];
    output.broken_pixel_count = impl_->broken_pixel_count;
    output.outlier_pixel_count = impl_->outlier_pixel_count;
    std::copy(std::begin(impl_->parameters.brokenPixels),
              std::end(impl_->parameters.brokenPixels),
              output.broken_pixels.begin());
    std::copy(std::begin(impl_->parameters.outlierPixels),
              std::end(impl_->parameters.outlierPixels),
              output.outlier_pixels.begin());

    const auto calculation_start = std::chrono::steady_clock::now();
    for (std::size_t page = 0; page < 2; ++page) {
        auto &frame_data = decoded.frame_data[page];
        output.subpage_vdd[page] =
            MLX90640_GetVdd(frame_data.data(), &impl_->parameters);
        output.subpage_ta_c[page] =
            MLX90640_GetTa(frame_data.data(), &impl_->parameters);
        const float reflected_temperature = output.subpage_ta_c[page] +
            config.reflected_temperature_offset_c;
        MLX90640_CalculateTo(frame_data.data(), &impl_->parameters,
                            config.emissivity, reflected_temperature,
                            output.temperature_c.data());
        MLX90640_GetImage(frame_data.data(), &impl_->parameters,
                         output.image.data());
    }

    if (config.correct_bad_pixels) {
        MLX90640_BadPixelsCorrection(impl_->parameters.brokenPixels,
                                     output.temperature_c.data(), 1,
                                     &impl_->parameters);
        MLX90640_BadPixelsCorrection(impl_->parameters.outlierPixels,
                                     output.temperature_c.data(), 1,
                                     &impl_->parameters);
        MLX90640_BadPixelsCorrection(impl_->parameters.brokenPixels,
                                     output.image.data(), 1,
                                     &impl_->parameters);
        MLX90640_BadPixelsCorrection(impl_->parameters.outlierPixels,
                                     output.image.data(), 1,
                                     &impl_->parameters);
    }
    output.calculation_time_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - calculation_start)
            .count());

    output.ta_c =
        (output.subpage_ta_c[0] + output.subpage_ta_c[1]) / 2.0F;
    output.vdd =
        (output.subpage_vdd[0] + output.subpage_vdd[1]) / 2.0F;
    output.min_temperature_c = std::numeric_limits<float>::infinity();
    output.max_temperature_c = -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < kMlx90640Pixels; ++index) {
        if (std::isfinite(output.temperature_c[index])) {
            ++output.finite_temperature_pixels;
            output.min_temperature_c = std::min(
                output.min_temperature_c, output.temperature_c[index]);
            output.max_temperature_c = std::max(
                output.max_temperature_c, output.temperature_c[index]);
        }
        if (std::isfinite(output.image[index]))
            ++output.finite_image_pixels;
    }

    const bool scalar_values_valid = std::isfinite(output.ta_c) &&
        std::isfinite(output.vdd) &&
        std::isfinite(output.min_temperature_c) &&
        std::isfinite(output.max_temperature_c);
    const bool complete_matrix =
        output.finite_temperature_pixels == kMlx90640Pixels &&
        output.finite_image_pixels == kMlx90640Pixels;
    if (!scalar_values_valid || (config.correct_bad_pixels &&
                                  !complete_matrix)) {
        std::ostringstream message;
        message << "non-finite thermal result: temperature="
                << output.finite_temperature_pixels << "/"
                << kMlx90640Pixels << " image="
                << output.finite_image_pixels << "/" << kMlx90640Pixels;
        set_error(error, message.str());
        return false;
    }

    *result = std::move(output);
    return true;
}

bool Mlx90640Math::initialized() const
{
    return impl_->initialized;
}

}  // namespace p2
