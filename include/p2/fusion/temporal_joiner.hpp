#pragma once

#include "p2/capture/frame_types.hpp"
#include "p2/inference/person_detector.hpp"
#include "p2/sync/frame_synchronizer.hpp"
#include "p2/thermal/mlx90640_math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace p2 {

struct VisibleFusionFrame {
    VisibleFrameEvent event;
    PersonInferenceTiming timing;
    std::vector<PersonDetection> detections;
    std::uint64_t inference_done_ns = 0;
};

struct ThermalFusionFrame {
    ThermalFrameEvent event;
    std::array<float, kMlx90640Pixels> temperature_c{};
    float ambient_temperature_c = 0.0F;
    std::uint64_t processing_done_ns = 0;
};

struct FusionInputPair {
    MatchRecord match;
    VisibleFusionFrame visible;
    ThermalFusionFrame thermal;
};

struct TemporalFusionJoinerConfig {
    SynchronizerConfig synchronizer;
    std::size_t max_visible_results = 96;
    std::size_t max_thermal_results = 24;
};

struct TemporalFusionJoinerStats {
    SynchronizerStats synchronizer;
    std::uint64_t emitted_pairs = 0;
    std::uint64_t missing_visible_results = 0;
    std::uint64_t missing_thermal_results = 0;
    std::uint64_t visible_result_drops = 0;
    std::uint64_t thermal_result_drops = 0;
    std::size_t visible_results_pending = 0;
    std::size_t thermal_results_pending = 0;
};

class TemporalFusionJoiner {
public:
    explicit TemporalFusionJoiner(TemporalFusionJoinerConfig config = {});
    ~TemporalFusionJoiner();

    TemporalFusionJoiner(const TemporalFusionJoiner &) = delete;
    TemporalFusionJoiner &operator=(const TemporalFusionJoiner &) = delete;

    std::vector<FusionInputPair> push_visible(VisibleFusionFrame frame);
    std::vector<FusionInputPair> push_thermal(ThermalFusionFrame frame);
    std::vector<FusionInputPair> flush();
    TemporalFusionJoinerStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace p2
