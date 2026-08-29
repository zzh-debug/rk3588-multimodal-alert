#include "p2/calibration/assistant.hpp"
#include "p2/calibration/cross_spectral.hpp"
#include "p2/capture/device_discovery.hpp"
#include "p2/capture/thermal_capture.hpp"
#include "p2/capture/visible_capture.hpp"
#include "p2/thermal/colormap.hpp"
#include "p2/thermal/mlx90640_math.hpp"
#include "p2/thermal/temporal_filter.hpp"

#include <QApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QTransform>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>

#ifndef P2_GIT_COMMIT
#define P2_GIT_COMMIT "unknown"
#endif

namespace {

constexpr const char *kProjectOneCommit =
    "c140532b82723bdfe1396b51304f2dfe9c4c73e7";
constexpr std::uint32_t kVisibleWidth = 3840;
constexpr std::uint32_t kVisibleHeight = 2160;
constexpr std::size_t kFitPointCount = 9;
constexpr std::size_t kValidationPointCount = 4;
constexpr std::size_t kConsecutiveThermalFailureLimit = 8;

const std::array<p2::Point2d, kFitPointCount> kFitGuides{{
    {0.20, 0.20}, {0.50, 0.20}, {0.80, 0.20},
    {0.20, 0.50}, {0.50, 0.50}, {0.80, 0.50},
    {0.20, 0.80}, {0.50, 0.80}, {0.80, 0.80},
}};

const std::array<p2::Point2d, kValidationPointCount> kValidationGuides{{
    {0.35, 0.35}, {0.65, 0.35}, {0.35, 0.65}, {0.65, 0.65},
}};

struct Options {
    std::string visible;
    std::string thermal;
    std::string eeprom =
        "/sys/bus/nvmem/devices/zzh_mlx90640_eeprom-5-33/nvmem";
    std::string output = "/tmp/p2-calibration-gui";
    std::uint64_t maximum_skew_ns = 100'000'000ULL;
    std::size_t background_frames = 16;
    std::size_t stable_frames = 4;
    double maximum_stability_deviation_pixels = 0.75;
    p2::ThermalHotspotConfig hotspot;
    p2::ImageDisplayTransform visible_transform{
        p2::RightAngleRotation::k0, false, true};
    p2::ImageDisplayTransform thermal_transform{
        p2::RightAngleRotation::kClockwise90, false, false};
};

void usage(const char *program)
{
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --visible PATH              override discovered RKISP mainpath\n"
        << "  --thermal PATH              override discovered ZMLX node\n"
        << "  --eeprom FILE               MLX90640 NVMEM path\n"
        << "  --output DIR                output directory\n"
        << "  --max-skew-ms N             following-frame gate, default 100\n"
        << "  --background-frames N       background average count, default 16\n"
        << "  --stable-frames N           frames per point, default 4\n"
        << "  --minimum-delta-c V         foreground delta, default 2.5\n"
        << "  --minimum-peak-delta-c V    peak delta, default 6.0\n"
        << "  --visible-rotation DEG      0/90/180/270, default 0\n"
        << "  --visible-flip-h 0|1        display-space horizontal flip\n"
        << "  --visible-flip-v 0|1        display-space vertical flip, default 1\n"
        << "  --thermal-rotation DEG      0/90/180/270, default 90\n"
        << "  --thermal-flip-h 0|1        display-space horizontal flip\n"
        << "  --thermal-flip-v 0|1        display-space vertical flip\n";
}

bool parse_u64(const char *text, std::uint64_t *value)
{
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *value = parsed;
    return true;
}

bool parse_size(const char *text, std::size_t *value)
{
    std::uint64_t parsed = 0;
    if (!parse_u64(text, &parsed) ||
        parsed > std::numeric_limits<std::size_t>::max())
        return false;
    *value = static_cast<std::size_t>(parsed);
    return true;
}

bool parse_double(const char *text, double *value)
{
    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' ||
        !std::isfinite(parsed))
        return false;
    *value = parsed;
    return true;
}

bool parse_bool(const char *text, bool *value)
{
    if (std::strcmp(text, "0") == 0) {
        *value = false;
        return true;
    }
    if (std::strcmp(text, "1") == 0) {
        *value = true;
        return true;
    }
    return false;
}

bool parse_rotation(const char *text, p2::RightAngleRotation *rotation)
{
    if (std::strcmp(text, "0") == 0) {
        *rotation = p2::RightAngleRotation::k0;
        return true;
    }
    if (std::strcmp(text, "90") == 0) {
        *rotation = p2::RightAngleRotation::kClockwise90;
        return true;
    }
    if (std::strcmp(text, "180") == 0) {
        *rotation = p2::RightAngleRotation::k180;
        return true;
    }
    if (std::strcmp(text, "270") == 0) {
        *rotation = p2::RightAngleRotation::kClockwise270;
        return true;
    }
    return false;
}

bool parse_options(int argc, char **argv, Options *options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (index + 1 >= argc)
            return false;
        const char *value = argv[++index];
        if (argument == "--visible")
            options->visible = value;
        else if (argument == "--thermal")
            options->thermal = value;
        else if (argument == "--eeprom")
            options->eeprom = value;
        else if (argument == "--output")
            options->output = value;
        else if (argument == "--max-skew-ms") {
            std::uint64_t milliseconds = 0;
            if (!parse_u64(value, &milliseconds) || milliseconds == 0 ||
                milliseconds >
                    std::numeric_limits<std::uint64_t>::max() / 1'000'000ULL)
                return false;
            options->maximum_skew_ns = milliseconds * 1'000'000ULL;
        } else if (argument == "--background-frames") {
            if (!parse_size(value, &options->background_frames))
                return false;
        } else if (argument == "--stable-frames") {
            if (!parse_size(value, &options->stable_frames))
                return false;
        } else if (argument == "--minimum-delta-c") {
            double parsed = 0.0;
            if (!parse_double(value, &parsed))
                return false;
            options->hotspot.minimum_delta_c = static_cast<float>(parsed);
        } else if (argument == "--minimum-peak-delta-c") {
            double parsed = 0.0;
            if (!parse_double(value, &parsed))
                return false;
            options->hotspot.minimum_peak_delta_c =
                static_cast<float>(parsed);
        } else if (argument == "--visible-rotation") {
            if (!parse_rotation(value, &options->visible_transform.rotation))
                return false;
        } else if (argument == "--visible-flip-h") {
            if (!parse_bool(value,
                            &options->visible_transform.flip_horizontal))
                return false;
        } else if (argument == "--visible-flip-v") {
            if (!parse_bool(value,
                            &options->visible_transform.flip_vertical))
                return false;
        } else if (argument == "--thermal-rotation") {
            if (!parse_rotation(value, &options->thermal_transform.rotation))
                return false;
        } else if (argument == "--thermal-flip-h") {
            if (!parse_bool(value,
                            &options->thermal_transform.flip_horizontal))
                return false;
        } else if (argument == "--thermal-flip-v") {
            if (!parse_bool(value,
                            &options->thermal_transform.flip_vertical))
                return false;
        } else {
            return false;
        }
    }
    return !options->eeprom.empty() && !options->output.empty() &&
        options->background_frames >= 4 &&
        options->background_frames <= 128 &&
        options->stable_frames >= 2 && options->stable_frames <= 16 &&
        options->maximum_stability_deviation_pixels >= 0.0;
}

bool read_file(const std::string &path, std::vector<std::uint8_t> *bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    bytes->assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool make_directories(const std::string &path)
{
    if (path.empty())
        return false;
    std::string current = path.front() == '/' ? "/" : "";
    std::size_t start = path.front() == '/' ? 1 : 0;
    while (start <= path.size()) {
        const std::size_t separator = path.find('/', start);
        const std::string component = path.substr(
            start, separator == std::string::npos ? std::string::npos
                                                   : separator - start);
        if (!component.empty()) {
            if (!current.empty() && current.back() != '/')
                current += '/';
            current += component;
            if (mkdir(current.c_str(), 0755) < 0 && errno != EEXIST)
                return false;
        }
        if (separator == std::string::npos)
            break;
        start = separator + 1;
    }
    return true;
}

const char *rotation_name(p2::RightAngleRotation rotation)
{
    switch (rotation) {
    case p2::RightAngleRotation::k0:
        return "0";
    case p2::RightAngleRotation::kClockwise90:
        return "90cw";
    case p2::RightAngleRotation::k180:
        return "180";
    case p2::RightAngleRotation::kClockwise270:
        return "270cw";
    }
    return "unknown";
}

enum class SessionPhase {
    kNeedBackground,
    kCollectingBackground,
    kFitting,
    kValidation,
    kComplete,
    kFailed,
};

struct PendingThermal {
    bool valid = false;
    std::uint64_t timestamp_ns = 0;
    p2::ThermalHotspot hotspot;
};

struct SessionSnapshot {
    SessionPhase phase = SessionPhase::kNeedBackground;
    std::string status;
    std::size_t background_frames = 0;
    std::size_t background_required = 0;
    std::size_t fit_points = 0;
    std::size_t validation_points = 0;
    std::size_t stable_frames = 0;
    std::size_t stable_required = 0;
    bool capture_armed = false;
    bool has_guide = false;
    p2::Point2d guide_normalized;
    bool has_hotspot = false;
    p2::ThermalHotspot hotspot;
    std::string hotspot_error;
    p2::HomographyFitResult fit;
    p2::MappingErrorDistribution validation;
};

struct SessionResult {
    std::vector<p2::CrossSpectralCorrespondence> fit_points;
    std::vector<p2::CrossSpectralCorrespondence> validation_points;
    p2::HomographyFitResult fit;
    p2::MappingErrorDistribution validation;
};

class CalibrationSession {
public:
    CalibrationSession(const Options &options,
                       p2::ImageDimensions visible_dimensions)
        : options_(options), visible_dimensions_(visible_dimensions)
    {
        background_sum_.fill(0.0);
        background_c_.fill(0.0F);
    }

    void start_background()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_locked();
        phase_ = SessionPhase::kCollectingBackground;
        status_ = "请移走暖目标，正在采集背景";
    }

    bool arm_current_point(std::string *error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ != SessionPhase::kFitting &&
            phase_ != SessionPhase::kValidation) {
            if (error != nullptr)
                *error = "请先完成背景采集";
            return false;
        }
        if (capture_armed_) {
            if (error != nullptr)
                *error = "当前点正在采集中";
            return false;
        }
        stable_samples_.clear();
        pending_ = PendingThermal{};
        capture_armed_ = true;
        status_ = "保持暖目标静止，正在采集当前点";
        return true;
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_locked();
    }

    void fail(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == SessionPhase::kComplete)
            return;
        phase_ = SessionPhase::kFailed;
        capture_armed_ = false;
        pending_ = PendingThermal{};
        status_ = message;
    }

    void on_thermal(
        const p2::ThermalFrameEvent &event,
        const std::array<float, p2::kThermalColumns * p2::kThermalRows>
            &temperature_c)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_hotspot_valid_ = false;
        if (phase_ == SessionPhase::kCollectingBackground) {
            for (std::size_t index = 0; index < temperature_c.size(); ++index)
                background_sum_[index] += temperature_c[index];
            ++background_frames_;
            if (background_frames_ >= options_.background_frames) {
                for (std::size_t index = 0;
                     index < background_c_.size(); ++index) {
                    background_c_[index] = static_cast<float>(
                        background_sum_[index] /
                        static_cast<double>(background_frames_));
                }
                phase_ = SessionPhase::kFitting;
                status_ = "背景完成：把暖目标移到第1个十字后采集";
            }
            return;
        }
        if (phase_ != SessionPhase::kFitting &&
            phase_ != SessionPhase::kValidation)
            return;

        p2::ThermalHotspot hotspot;
        std::string detection_error;
        if (!p2::detect_thermal_hotspot(
                temperature_c, background_c_, options_.hotspot,
                &hotspot, &detection_error)) {
            latest_hotspot_error_ = detection_error;
            if (capture_armed_)
                status_ = "等待合格热目标：" + detection_error;
            return;
        }
        latest_hotspot_ = hotspot;
        latest_hotspot_valid_ = true;
        latest_hotspot_error_.clear();
        if (capture_armed_ && !pending_.valid) {
            pending_.valid = true;
            pending_.timestamp_ns = event.timestamp_ns;
            pending_.hotspot = hotspot;
        }
    }

    void on_visible(const p2::VisibleFrameEvent &event)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capture_armed_ || !pending_.valid)
            return;
        if (event.timestamp_ns < pending_.timestamp_ns)
            return;
        const std::uint64_t skew =
            event.timestamp_ns - pending_.timestamp_ns;
        if (skew > options_.maximum_skew_ns) {
            pending_ = PendingThermal{};
            ++skew_rejections_;
            status_ = "时间差超限，保持目标静止后自动重试";
            return;
        }
        stable_samples_.push_back(pending_.hotspot);
        pending_ = PendingThermal{};
        if (stable_samples_.size() < options_.stable_frames) {
            status_ = "当前点稳定帧 " +
                std::to_string(stable_samples_.size()) + "/" +
                std::to_string(options_.stable_frames);
            return;
        }

        p2::StableHotspot stable;
        std::string error;
        if (!p2::summarize_stable_hotspots(
                stable_samples_,
                options_.maximum_stability_deviation_pixels,
                &stable, &error)) {
            stable_samples_.clear();
            status_ = "目标不稳定，自动重新采样：" + error;
            return;
        }
        p2::Point2d visible;
        const p2::Point2d guide = guide_locked();
        if (!p2::display_normalized_to_source(
                visible_dimensions_, options_.visible_transform,
                guide, &visible, &error)) {
            fail_locked("十字坐标换算失败：" + error);
            return;
        }
        const p2::CrossSpectralCorrespondence correspondence{
            stable.centroid, visible};
        if (phase_ == SessionPhase::kFitting) {
            fit_points_.push_back(correspondence);
            capture_armed_ = false;
            stable_samples_.clear();
            if (fit_points_.size() == kFitPointCount) {
                if (!p2::fit_cross_spectral_homography(
                        fit_points_, &fit_, &error)) {
                    fail_locked("单应拟合失败：" + error);
                    return;
                }
                phase_ = SessionPhase::kValidation;
                status_ = "拟合完成：开始独立验证点1";
            } else {
                status_ = "拟合点完成，移动到下一个十字";
            }
            return;
        }

        validation_points_.push_back(correspondence);
        capture_armed_ = false;
        stable_samples_.clear();
        if (validation_points_.size() == kValidationPointCount) {
            if (!p2::calculate_mapping_errors(
                    fit_.thermal_to_visible, validation_points_,
                    &validation_, &error)) {
                fail_locked("独立验证失败：" + error);
                return;
            }
            phase_ = SessionPhase::kComplete;
            status_ = "验证完成：检查误差与距离后保存配置";
        } else {
            status_ = "验证点完成，移动到下一个十字";
        }
    }

    SessionSnapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SessionSnapshot snapshot;
        snapshot.phase = phase_;
        snapshot.status = status_;
        snapshot.background_frames = background_frames_;
        snapshot.background_required = options_.background_frames;
        snapshot.fit_points = fit_points_.size();
        snapshot.validation_points = validation_points_.size();
        snapshot.stable_frames = stable_samples_.size();
        snapshot.stable_required = options_.stable_frames;
        snapshot.capture_armed = capture_armed_;
        snapshot.has_hotspot = latest_hotspot_valid_;
        snapshot.hotspot = latest_hotspot_;
        snapshot.hotspot_error = latest_hotspot_error_;
        snapshot.fit = fit_;
        snapshot.validation = validation_;
        if (phase_ == SessionPhase::kFitting ||
            phase_ == SessionPhase::kValidation) {
            snapshot.has_guide = true;
            snapshot.guide_normalized = guide_locked();
        }
        return snapshot;
    }

    bool result(SessionResult *result, std::string *error) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (result == nullptr) {
            if (error != nullptr)
                *error = "result output pointer is null";
            return false;
        }
        if (phase_ != SessionPhase::kComplete) {
            if (error != nullptr)
                *error = "calibration session is not complete";
            return false;
        }
        result->fit_points = fit_points_;
        result->validation_points = validation_points_;
        result->fit = fit_;
        result->validation = validation_;
        return true;
    }

private:
    p2::Point2d guide_locked() const
    {
        if (phase_ == SessionPhase::kValidation)
            return kValidationGuides.at(validation_points_.size());
        return kFitGuides.at(fit_points_.size());
    }

    void reset_locked()
    {
        phase_ = SessionPhase::kNeedBackground;
        status_ = "先移走暖目标并采集背景";
        background_sum_.fill(0.0);
        background_c_.fill(0.0F);
        background_frames_ = 0;
        fit_points_.clear();
        validation_points_.clear();
        fit_ = p2::HomographyFitResult{};
        validation_ = p2::MappingErrorDistribution{};
        stable_samples_.clear();
        pending_ = PendingThermal{};
        capture_armed_ = false;
        latest_hotspot_valid_ = false;
        latest_hotspot_error_.clear();
        skew_rejections_ = 0;
    }

    void fail_locked(const std::string &message)
    {
        phase_ = SessionPhase::kFailed;
        capture_armed_ = false;
        pending_ = PendingThermal{};
        status_ = message;
    }

    const Options options_;
    const p2::ImageDimensions visible_dimensions_;
    mutable std::mutex mutex_;
    SessionPhase phase_ = SessionPhase::kNeedBackground;
    std::string status_ = "先移走暖目标并采集背景";
    std::array<double, p2::kThermalColumns * p2::kThermalRows>
        background_sum_{};
    std::array<float, p2::kThermalColumns * p2::kThermalRows>
        background_c_{};
    std::size_t background_frames_ = 0;
    bool capture_armed_ = false;
    PendingThermal pending_;
    std::vector<p2::ThermalHotspot> stable_samples_;
    std::vector<p2::CrossSpectralCorrespondence> fit_points_;
    std::vector<p2::CrossSpectralCorrespondence> validation_points_;
    p2::HomographyFitResult fit_;
    p2::MappingErrorDistribution validation_;
    bool latest_hotspot_valid_ = false;
    p2::ThermalHotspot latest_hotspot_;
    std::string latest_hotspot_error_;
    std::uint64_t skew_rejections_ = 0;
};

struct PreviewSnapshot {
    std::vector<std::uint8_t> visible_rgb;
    int visible_width = 0;
    int visible_height = 0;
    std::array<p2::Rgb8, p2::kThermalColumns * p2::kThermalRows>
        thermal_rgb{};
    bool visible_valid = false;
    bool thermal_valid = false;
};

class PreviewStore {
public:
    void update_visible(std::vector<std::uint8_t> rgb, int width, int height)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        visible_rgb_ = std::move(rgb);
        visible_width_ = width;
        visible_height_ = height;
        visible_valid_ = true;
    }

    void update_thermal(const p2::ThermalColorMapResult &colors)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        thermal_rgb_ = colors.rgb;
        thermal_valid_ = true;
    }

    PreviewSnapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        PreviewSnapshot snapshot;
        snapshot.visible_rgb = visible_rgb_;
        snapshot.visible_width = visible_width_;
        snapshot.visible_height = visible_height_;
        snapshot.thermal_rgb = thermal_rgb_;
        snapshot.visible_valid = visible_valid_;
        snapshot.thermal_valid = thermal_valid_;
        return snapshot;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> visible_rgb_;
    int visible_width_ = 0;
    int visible_height_ = 0;
    std::array<p2::Rgb8, p2::kThermalColumns * p2::kThermalRows>
        thermal_rgb_{};
    bool visible_valid_ = false;
    bool thermal_valid_ = false;
};

class VisiblePreviewRenderer {
public:
    VisiblePreviewRenderer(
        p2::ImageDimensions source,
        p2::ImageDisplayTransform transform,
        int output_width,
        int output_height)
        : source_(source), transform_(transform),
          output_width_(output_width), output_height_(output_height)
    {
        lookup_.resize(
            static_cast<std::size_t>(output_width_) * output_height_);
        std::string error;
        for (int y = 0; y < output_height_; ++y) {
            for (int x = 0; x < output_width_; ++x) {
                const p2::Point2d normalized{
                    output_width_ > 1 ?
                        static_cast<double>(x) /
                            static_cast<double>(output_width_ - 1) : 0.0,
                    output_height_ > 1 ?
                        static_cast<double>(y) /
                            static_cast<double>(output_height_ - 1) : 0.0,
                };
                p2::Point2d source_point;
                if (!p2::display_normalized_to_source(
                        source_, transform_, normalized,
                        &source_point, &error)) {
                    valid_ = false;
                    error_ = error;
                    return;
                }
                lookup_[
                    static_cast<std::size_t>(y) * output_width_ + x] = {
                        static_cast<std::uint32_t>(std::lround(source_point.x)),
                        static_cast<std::uint32_t>(std::lround(source_point.y)),
                    };
            }
        }
        valid_ = true;
    }

    bool render(const p2::VisibleFrameView &frame,
                std::vector<std::uint8_t> *rgb,
                std::string *error) const
    {
        if (!valid_) {
            if (error != nullptr)
                *error = error_;
            return false;
        }
        if (frame.data == nullptr || frame.width != source_.width ||
            frame.height != source_.height ||
            frame.bytes_per_line < frame.width) {
            if (error != nullptr)
                *error = "visible preview frame geometry is invalid";
            return false;
        }
        const std::size_t y_bytes =
            static_cast<std::size_t>(frame.bytes_per_line) * frame.height;
        if (y_bytes >= frame.size) {
            if (error != nullptr)
                *error = "visible preview NV12 payload is truncated";
            return false;
        }
        const std::uint8_t *y_plane = frame.data;
        const std::uint8_t *uv_plane = frame.data + y_bytes;
        rgb->resize(
            static_cast<std::size_t>(output_width_) * output_height_ * 3);
        for (std::size_t index = 0; index < lookup_.size(); ++index) {
            const std::uint32_t x = lookup_[index].first;
            const std::uint32_t y = lookup_[index].second;
            const int luma =
                y_plane[static_cast<std::size_t>(y) *
                            frame.bytes_per_line + x];
            const std::size_t uv_offset =
                static_cast<std::size_t>(y / 2) * frame.bytes_per_line +
                (x & ~1U);
            if (y_bytes + uv_offset + 1 >= frame.size) {
                if (error != nullptr)
                    *error = "visible preview UV payload is truncated";
                return false;
            }
            const int u = static_cast<int>(uv_plane[uv_offset]) - 128;
            const int v = static_cast<int>(uv_plane[uv_offset + 1]) - 128;
            const auto clamp = [](int value) {
                return static_cast<std::uint8_t>(
                    std::clamp(value, 0, 255));
            };
            (*rgb)[index * 3] =
                clamp(luma + ((359 * v) >> 8));
            (*rgb)[index * 3 + 1] =
                clamp(luma - ((88 * u + 183 * v) >> 8));
            (*rgb)[index * 3 + 2] =
                clamp(luma + ((454 * u) >> 8));
        }
        return true;
    }

private:
    p2::ImageDimensions source_;
    p2::ImageDisplayTransform transform_;
    int output_width_ = 0;
    int output_height_ = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> lookup_;
    bool valid_ = false;
    std::string error_;
};

QImage thermal_image(
    const PreviewSnapshot &preview,
    const p2::ImageDisplayTransform &transform)
{
    QImage raw(
        static_cast<int>(p2::kThermalColumns),
        static_cast<int>(p2::kThermalRows),
        QImage::Format_RGB888);
    for (std::size_t row = 0; row < p2::kThermalRows; ++row) {
        std::uint8_t *line = raw.scanLine(static_cast<int>(row));
        for (std::size_t column = 0; column < p2::kThermalColumns; ++column) {
            const p2::Rgb8 pixel =
                preview.thermal_rgb[row * p2::kThermalColumns + column];
            line[column * 3] = pixel.red;
            line[column * 3 + 1] = pixel.green;
            line[column * 3 + 2] = pixel.blue;
        }
    }
    int angle = 0;
    if (transform.rotation == p2::RightAngleRotation::kClockwise90)
        angle = 90;
    else if (transform.rotation == p2::RightAngleRotation::k180)
        angle = 180;
    else if (transform.rotation == p2::RightAngleRotation::kClockwise270)
        angle = 270;
    QImage displayed = angle == 0 ?
        raw : raw.transformed(QTransform().rotate(angle));
    if (transform.flip_horizontal || transform.flip_vertical)
        displayed = displayed.mirrored(
            transform.flip_horizontal, transform.flip_vertical);
    return displayed;
}

bool valid_distances(int minimum, int nominal, int maximum)
{
    return minimum > 0 && nominal >= minimum && maximum >= nominal;
}

bool write_results(
    const Options &options,
    const SessionResult &result,
    int minimum_distance_mm,
    int nominal_distance_mm,
    int maximum_distance_mm,
    int maximum_allowed_error_px,
    std::string *written_config,
    std::string *error)
{
    if (!valid_distances(
            minimum_distance_mm, nominal_distance_mm, maximum_distance_mm)) {
        *error = "距离范围无效";
        return false;
    }
    if (result.fit.max_reprojection_error_px >
            static_cast<double>(maximum_allowed_error_px) ||
        result.validation.maximum_px >
            static_cast<double>(maximum_allowed_error_px)) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(1)
                << "误差超过门限：拟合最大"
                << result.fit.max_reprojection_error_px
                << "px，验证最大" << result.validation.maximum_px
                << "px，门限" << maximum_allowed_error_px << "px";
        *error = message.str();
        return false;
    }
    if (!make_directories(options.output)) {
        *error = "无法创建输出目录";
        return false;
    }
    const std::string calibration_id =
        "gui-" + QDateTime::currentDateTime()
            .toString("yyyyMMdd-HHmmss").toStdString();
    p2::CrossSpectralCalibration calibration;
    calibration.schema = p2::kCrossSpectralCalibrationSchema;
    calibration.state = p2::CalibrationState::calibrated;
    calibration.calibration_id = calibration_id;
    calibration.thermal_model = p2::Mlx90640Model::esf_baa;
    calibration.thermal_width = p2::kThermalColumns;
    calibration.thermal_height = p2::kThermalRows;
    calibration.visible_width = kVisibleWidth;
    calibration.visible_height = kVisibleHeight;
    calibration.nominal_distance_mm = nominal_distance_mm;
    calibration.valid_distance_min_mm = minimum_distance_mm;
    calibration.valid_distance_max_mm = maximum_distance_mm;
    calibration.thermal_to_visible = result.fit.thermal_to_visible;
    calibration.max_reprojection_error_px =
        result.fit.max_reprojection_error_px;

    const std::string config_path =
        options.output + "/" + calibration_id + ".conf";
    std::ofstream config(config_path);
    std::string write_error;
    if (!config || !p2::write_cross_spectral_calibration(
                       config, calibration, &write_error)) {
        *error = "写入标定配置失败：" + write_error;
        return false;
    }
    std::ofstream points(options.output + "/correspondences.csv");
    if (!points) {
        *error = "写入对应点失败";
        return false;
    }
    points << "phase,index,thermal_x,thermal_y,visible_x,visible_y\n"
           << std::fixed << std::setprecision(6);
    for (std::size_t index = 0; index < result.fit_points.size(); ++index) {
        const auto &point = result.fit_points[index];
        points << "fit," << index << ',' << point.thermal.x << ','
               << point.thermal.y << ',' << point.visible.x << ','
               << point.visible.y << '\n';
    }
    for (std::size_t index = 0;
         index < result.validation_points.size(); ++index) {
        const auto &point = result.validation_points[index];
        points << "validation," << index << ',' << point.thermal.x << ','
               << point.thermal.y << ',' << point.visible.x << ','
               << point.visible.y << '\n';
    }
    if (!points) {
        *error = "写入对应点时发生错误";
        return false;
    }

    std::ofstream report(options.output + "/calibration-report.json");
    if (!report) {
        *error = "写入标定报告失败";
        return false;
    }
    report << std::fixed << std::setprecision(6)
           << "{\n"
           << "  \"schema\": \"p2.calibration-gui-report.v1\",\n"
           << "  \"result\": \"PASS\",\n"
           << "  \"application_commit\": \"" << P2_GIT_COMMIT
           << "\",\n"
           << "  \"project_one_commit\": \"" << kProjectOneCommit
           << "\",\n"
           << "  \"calibration_id\": \"" << calibration_id << "\",\n"
           << "  \"thermal_model\": \"MLX90640-ESF-BAA\",\n"
           << "  \"visible_transform\": {\"rotation\": \""
           << rotation_name(options.visible_transform.rotation)
           << "\", \"flip_h\": "
           << (options.visible_transform.flip_horizontal ? "true" : "false")
           << ", \"flip_v\": "
           << (options.visible_transform.flip_vertical ? "true" : "false")
           << "},\n"
           << "  \"thermal_transform\": {\"rotation\": \""
           << rotation_name(options.thermal_transform.rotation)
           << "\", \"flip_h\": "
           << (options.thermal_transform.flip_horizontal ? "true" : "false")
           << ", \"flip_v\": "
           << (options.thermal_transform.flip_vertical ? "true" : "false")
           << "},\n"
           << "  \"hotspot_detector\": {\"minimum_delta_c\": "
           << options.hotspot.minimum_delta_c
           << ", \"minimum_peak_delta_c\": "
           << options.hotspot.minimum_peak_delta_c
           << ", \"centroid_peak_fraction\": "
           << options.hotspot.centroid_peak_fraction
           << ", \"minimum_core_pixels\": "
           << options.hotspot.minimum_pixels
           << ", \"maximum_core_pixels\": "
           << options.hotspot.maximum_pixels
           << ", \"maximum_secondary_peak_ratio\": "
           << options.hotspot.maximum_secondary_peak_ratio
           << ", \"maximum_centroid_to_peak_distance_pixels\": "
           << options.hotspot.maximum_centroid_to_peak_distance_pixels
           << "},\n"
           << "  \"fit_points\": " << result.fit_points.size() << ",\n"
           << "  \"validation_points\": "
           << result.validation_points.size() << ",\n"
           << "  \"fit_rmse_px\": "
           << result.fit.root_mean_square_error_px << ",\n"
           << "  \"fit_max_px\": "
           << result.fit.max_reprojection_error_px << ",\n"
           << "  \"validation_rmse_px\": "
           << result.validation.root_mean_square_px << ",\n"
           << "  \"validation_p50_px\": "
           << result.validation.p50_px << ",\n"
           << "  \"validation_p95_px\": "
           << result.validation.p95_px << ",\n"
           << "  \"validation_max_px\": "
           << result.validation.maximum_px << ",\n"
           << "  \"maximum_allowed_error_px\": "
           << maximum_allowed_error_px << ",\n"
           << "  \"distance_mm\": {\"min\": "
           << minimum_distance_mm << ", \"nominal\": "
           << nominal_distance_mm << ", \"max\": "
           << maximum_distance_mm << "}\n"
           << "}\n";
    if (!report) {
        *error = "写入标定报告时发生错误";
        return false;
    }
    *written_config = config_path;
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    QApplication app(argc, argv);

    p2::DiscoveredDevices devices;
    std::string error;
    if (options.visible.empty() && options.thermal.empty()) {
        if (!p2::discover_devices(&devices, &error)) {
            QMessageBox::critical(nullptr, "设备发现失败",
                                  QString::fromStdString(error));
            return EXIT_FAILURE;
        }
    } else {
        if (options.visible.empty()) {
            if (!p2::discover_visible_device(
                    &devices.visible, &devices.visible_media, &error)) {
                QMessageBox::critical(nullptr, "可见光设备失败",
                                      QString::fromStdString(error));
                return EXIT_FAILURE;
            }
        } else {
            devices.visible = options.visible;
        }
        if (options.thermal.empty()) {
            if (!p2::discover_thermal_device(&devices.thermal, &error)) {
                QMessageBox::critical(nullptr, "热成像设备失败",
                                      QString::fromStdString(error));
                return EXIT_FAILURE;
            }
        } else {
            devices.thermal = options.thermal;
        }
    }
    options.visible = devices.visible;
    options.thermal = devices.thermal;

    std::vector<std::uint8_t> eeprom;
    if (!read_file(options.eeprom, &eeprom)) {
        QMessageBox::critical(nullptr, "EEPROM失败",
                              "无法读取MLX90640 EEPROM");
        return EXIT_FAILURE;
    }
    p2::Mlx90640Math math;
    if (!math.initialize_eeprom_be(eeprom.data(), eeprom.size(), &error)) {
        QMessageBox::critical(nullptr, "EEPROM解析失败",
                              QString::fromStdString(error));
        return EXIT_FAILURE;
    }

    QScreen *screen = QGuiApplication::primaryScreen();
    const int screen_width = screen->geometry().width();
    const int screen_height = screen->geometry().height();
    const int controls_height = std::min(520, screen_height / 3);
    const int pane_height = (screen_height - controls_height) / 2;
    const int visible_width = screen_width;
    int visible_height = visible_width * 9 / 16;
    if (visible_height > pane_height)
        visible_height = pane_height;
    const int preview_width = std::max(2, visible_width);
    const int preview_height = std::max(2, visible_height);

    CalibrationSession session(
        options, {kVisibleWidth, kVisibleHeight});
    PreviewStore previews;
    VisiblePreviewRenderer visible_renderer(
        {kVisibleWidth, kVisibleHeight}, options.visible_transform,
        preview_width, preview_height);

    QWidget window;
    window.setWindowTitle("P2 跨光谱半自动标定");
    window.setStyleSheet(
        "QWidget { background:#111; color:#fff; font-size:22px; }"
        "QLabel { color:#fff; }"
        "QPushButton { min-height:64px; font-size:22px; "
        "background:#2b6cb0; border-radius:8px; padding:6px 12px; }"
        "QPushButton:disabled { background:#555; color:#aaa; }"
        "QSpinBox { min-height:54px; background:#222; padding:4px; }");

    QLabel *visible_view = new QLabel(&window);
    visible_view->setAlignment(Qt::AlignCenter);
    visible_view->setFixedSize(screen_width, pane_height);
    visible_view->setStyleSheet("background:#000;");
    QLabel *thermal_view = new QLabel(&window);
    thermal_view->setAlignment(Qt::AlignCenter);
    thermal_view->setFixedSize(screen_width, pane_height);
    thermal_view->setStyleSheet(
        "background:#000; border:3px solid #ff8800;");
    QLabel *status = new QLabel(&window);
    status->setWordWrap(true);
    status->setMinimumHeight(62);

    auto make_distance = [&window]() {
        QSpinBox *box = new QSpinBox(&window);
        box->setRange(0, 10000);
        box->setSuffix(" mm");
        box->setSpecialValueText("未设置");
        return box;
    };
    QSpinBox *minimum_distance = make_distance();
    QSpinBox *nominal_distance = make_distance();
    QSpinBox *maximum_distance = make_distance();
    QSpinBox *maximum_error = new QSpinBox(&window);
    maximum_error->setRange(1, 1000);
    maximum_error->setValue(120);
    maximum_error->setSuffix(" px");

    QPushButton *background = new QPushButton("采集背景", &window);
    QPushButton *capture = new QPushButton("采集当前点", &window);
    QPushButton *reset = new QPushButton("重置", &window);
    QPushButton *save = new QPushButton("保存配置", &window);
    QPushButton *quit = new QPushButton("退出", &window);

    QHBoxLayout *distance_row = new QHBoxLayout();
    distance_row->addWidget(new QLabel("近", &window));
    distance_row->addWidget(minimum_distance);
    distance_row->addWidget(new QLabel("标定", &window));
    distance_row->addWidget(nominal_distance);
    distance_row->addWidget(new QLabel("远", &window));
    distance_row->addWidget(maximum_distance);
    distance_row->addWidget(new QLabel("误差门限", &window));
    distance_row->addWidget(maximum_error);

    QHBoxLayout *button_row = new QHBoxLayout();
    button_row->addWidget(background);
    button_row->addWidget(capture);
    button_row->addWidget(reset);
    button_row->addWidget(save);
    button_row->addWidget(quit);

    QWidget *controls = new QWidget(&window);
    controls->setFixedHeight(controls_height);
    QVBoxLayout *controls_layout = new QVBoxLayout(controls);
    controls_layout->setContentsMargins(10, 6, 10, 6);
    controls_layout->addWidget(status);
    controls_layout->addLayout(distance_row);
    controls_layout->addLayout(button_row);

    QVBoxLayout *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(visible_view);
    layout->addWidget(thermal_view);
    layout->addWidget(controls);

    QObject::connect(background, &QPushButton::clicked, [&]() {
        session.start_background();
    });
    QObject::connect(capture, &QPushButton::clicked, [&]() {
        std::string message;
        if (!session.arm_current_point(&message))
            QMessageBox::warning(&window, "不能采集",
                                 QString::fromStdString(message));
    });
    QObject::connect(reset, &QPushButton::clicked, [&]() {
        session.reset();
    });
    QObject::connect(quit, &QPushButton::clicked, [&]() {
        window.close();
    });
    QObject::connect(save, &QPushButton::clicked, [&]() {
        SessionResult result;
        std::string message;
        if (!session.result(&result, &message)) {
            QMessageBox::warning(&window, "不能保存",
                                 QString::fromStdString(message));
            return;
        }
        std::string written;
        if (!write_results(
                options, result, minimum_distance->value(),
                nominal_distance->value(), maximum_distance->value(),
                maximum_error->value(), &written, &message)) {
            QMessageBox::warning(&window, "保存失败",
                                 QString::fromStdString(message));
            return;
        }
        QMessageBox::information(
            &window, "标定配置已保存",
            QString::fromStdString(written));
    });

    std::atomic<bool> stop{false};
    p2::VisibleCapture visible(
        {devices.visible, kVisibleWidth, kVisibleHeight, 6, 500});
    p2::ThermalCapture thermal({devices.thermal, 4, 1000});
    bool visible_ok = false;
    bool thermal_ok = false;
    std::string visible_error;
    std::string thermal_error;

    std::thread visible_thread([&]() {
        visible_ok = visible.run_frames(
            stop,
            [&](const p2::VisibleFrameView &frame) {
                session.on_visible(frame.event);
                if ((frame.event.sequence & 1U) != 0)
                    return;
                std::vector<std::uint8_t> rgb;
                std::string render_error;
                if (!visible_renderer.render(frame, &rgb, &render_error)) {
                    session.fail("可见光预览失败：" + render_error);
                    stop.store(true);
                    return;
                }
                previews.update_visible(
                    std::move(rgb), preview_width, preview_height);
            },
            &visible_error);
        if (!visible_ok && !stop.load())
            session.fail("可见光采集失败：" + visible_error);
    });

    std::thread thermal_thread([&]() {
        p2::ThermalMathConfig math_config;
        p2::ThermalTemporalFilter filter;
        p2::ThermalColorMapConfig color_config;
        std::size_t consecutive_processing_failures = 0;
        auto reject_thermal_frame = [&](const std::string &stage,
                                        const std::string &message) {
            ++consecutive_processing_failures;
            std::cerr << stage << " failed ("
                      << consecutive_processing_failures << "/"
                      << kConsecutiveThermalFailureLimit << "): "
                      << message << '\n';
            if (consecutive_processing_failures >=
                kConsecutiveThermalFailureLimit) {
                session.fail(
                    stage + "连续失败" +
                    std::to_string(kConsecutiveThermalFailureLimit) +
                    "帧：" + message);
                stop.store(true);
            }
        };
        thermal_ok = thermal.run(
            stop,
            [&](const p2::ThermalFramePayload &frame) {
                p2::ThermalMathResult calculated;
                std::string callback_error;
                if (!math.calculate(
                        frame, math_config, &calculated, &callback_error)) {
                    reject_thermal_frame("温度计算", callback_error);
                    return;
                }
                std::array<float, p2::kThermalColumns * p2::kThermalRows>
                    filtered{};
                if (!filter.process(
                        frame.event.timestamp_ns, calculated.temperature_c,
                        &filtered, &callback_error)) {
                    reject_thermal_frame("温度滤波", callback_error);
                    return;
                }
                p2::ThermalColorMapResult colors;
                if (!p2::apply_thermal_colormap(
                        filtered, color_config, &colors, &callback_error)) {
                    reject_thermal_frame("热图生成", callback_error);
                    return;
                }
                consecutive_processing_failures = 0;
                session.on_thermal(frame.event, calculated.temperature_c);
                previews.update_thermal(colors);
            },
            &thermal_error);
        if (!thermal_ok && !stop.load())
            session.fail("热成像采集失败：" + thermal_error);
    });

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        const PreviewSnapshot preview = previews.snapshot();
        const SessionSnapshot state = session.snapshot();
        if (preview.visible_valid) {
            QImage image(
                preview.visible_rgb.data(),
                preview.visible_width,
                preview.visible_height,
                preview.visible_width * 3,
                QImage::Format_RGB888);
            QImage annotated = image.copy();
            if (state.has_guide) {
                QPainter painter(&annotated);
                painter.setRenderHint(QPainter::Antialiasing);
                QPen pen(Qt::yellow);
                pen.setWidth(5);
                painter.setPen(pen);
                const QPointF center(
                    state.guide_normalized.x * (annotated.width() - 1),
                    state.guide_normalized.y * (annotated.height() - 1));
                painter.drawEllipse(center, 24, 24);
                painter.drawLine(
                    QPointF(center.x() - 40, center.y()),
                    QPointF(center.x() + 40, center.y()));
                painter.drawLine(
                    QPointF(center.x(), center.y() - 40),
                    QPointF(center.x(), center.y() + 40));
            }
            visible_view->setPixmap(
                QPixmap::fromImage(annotated).scaled(
                    visible_view->size(), Qt::KeepAspectRatio,
                    Qt::FastTransformation));
        }
        if (preview.thermal_valid) {
            QImage image = thermal_image(
                preview, options.thermal_transform);
            QImage enlarged = image.scaled(
                thermal_view->size(), Qt::KeepAspectRatio,
                Qt::FastTransformation);
            if (state.has_hotspot) {
                p2::Point2d centroid_normalized;
                p2::Point2d peak_normalized;
                std::string coordinate_error;
                if (p2::source_to_display_normalized(
                        {p2::kThermalColumns, p2::kThermalRows},
                        options.thermal_transform,
                        state.hotspot.centroid, &centroid_normalized,
                        &coordinate_error) &&
                    p2::source_to_display_normalized(
                        {p2::kThermalColumns, p2::kThermalRows},
                        options.thermal_transform,
                        state.hotspot.peak, &peak_normalized,
                        &coordinate_error)) {
                    QPainter painter(&enlarged);
                    QPen centroid_pen(Qt::white);
                    centroid_pen.setWidth(5);
                    painter.setPen(centroid_pen);
                    const QPointF center(
                        centroid_normalized.x * (enlarged.width() - 1),
                        centroid_normalized.y * (enlarged.height() - 1));
                    painter.drawEllipse(center, 18, 18);
                    QPen peak_pen(Qt::magenta);
                    peak_pen.setWidth(5);
                    painter.setPen(peak_pen);
                    const QPointF peak(
                        peak_normalized.x * (enlarged.width() - 1),
                        peak_normalized.y * (enlarged.height() - 1));
                    painter.drawLine(
                        QPointF(peak.x() - 14, peak.y()),
                        QPointF(peak.x() + 14, peak.y()));
                    painter.drawLine(
                        QPointF(peak.x(), peak.y() - 14),
                        QPointF(peak.x(), peak.y() + 14));
                }
            }
            thermal_view->setPixmap(QPixmap::fromImage(enlarged));
        }

        std::ostringstream message;
        message << state.status << "　背景 "
                << state.background_frames << "/"
                << state.background_required << "　拟合 "
                << state.fit_points << "/" << kFitPointCount
                << "　验证 " << state.validation_points << "/"
                << kValidationPointCount;
        if (state.capture_armed)
            message << "　稳定帧 " << state.stable_frames << "/"
                    << state.stable_required;
        if (state.has_hotspot) {
            message << std::fixed << std::setprecision(1)
                    << "　热峰Δ" << state.hotspot.peak_delta_c
                    << "℃　核心" << state.hotspot.pixel_count
                    << "px　峰心距"
                    << state.hotspot.centroid_to_peak_distance_pixels
                    << "px";
        } else if ((state.phase == SessionPhase::kFitting ||
                    state.phase == SessionPhase::kValidation) &&
                   !state.hotspot_error.empty()) {
            message << "　热目标：" << state.hotspot_error;
        }
        if (state.phase == SessionPhase::kComplete) {
            message << std::fixed << std::setprecision(1)
                    << "　拟合最大 "
                    << state.fit.max_reprojection_error_px
                    << "px　验证P95 " << state.validation.p95_px
                    << "px　验证最大 " << state.validation.maximum_px
                    << "px";
        }
        status->setText(QString::fromStdString(message.str()));
        capture->setEnabled(
            (state.phase == SessionPhase::kFitting ||
             state.phase == SessionPhase::kValidation) &&
            !state.capture_armed && state.has_hotspot);
        save->setEnabled(state.phase == SessionPhase::kComplete);
    });
    timer.start(50);

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        stop.store(true);
    });
    window.showFullScreen();
    const int application_result = app.exec();
    stop.store(true);
    visible_thread.join();
    thermal_thread.join();
    if (!visible_ok && !visible_error.empty())
        std::cerr << "visible: " << visible_error << '\n';
    if (!thermal_ok && !thermal_error.empty())
        std::cerr << "thermal: " << thermal_error << '\n';
    return application_result;
}
