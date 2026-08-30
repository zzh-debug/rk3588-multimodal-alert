#include "p2/fusion/temporal_joiner.hpp"

#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace p2 {

struct TemporalFusionJoiner::Impl {
    explicit Impl(TemporalFusionJoinerConfig value)
        : config(std::move(value)), synchronizer(config.synchronizer)
    {
        if (config.max_visible_results == 0 ||
            config.max_thermal_results == 0) {
            throw std::invalid_argument(
                "temporal fusion result bounds must be positive");
        }
    }

    std::vector<FusionInputPair> collect_locked()
    {
        std::vector<FusionInputPair> output;
        for (const MatchRecord &match : synchronizer.drain_matches()) {
            const auto visible = visible_results.find(match.visible_sequence);
            const auto thermal = thermal_results.find(match.thermal_sequence);
            if (visible == visible_results.end()) {
                ++stats.missing_visible_results;
                if (thermal != thermal_results.end())
                    thermal_results.erase(thermal);
                continue;
            }
            if (thermal == thermal_results.end()) {
                ++stats.missing_thermal_results;
                continue;
            }
            FusionInputPair pair;
            pair.match = match;
            pair.visible = visible->second;
            pair.thermal = std::move(thermal->second);
            thermal_results.erase(thermal);
            output.push_back(std::move(pair));
            ++stats.emitted_pairs;
        }
        while (!thermal_order.empty() &&
               thermal_results.count(thermal_order.front()) == 0U) {
            thermal_order.pop_front();
        }
        return output;
    }

    void prune_visible_locked()
    {
        while (visible_results.size() > config.max_visible_results) {
            if (visible_order.empty())
                break;
            const std::uint32_t sequence = visible_order.front();
            visible_order.pop_front();
            if (visible_results.erase(sequence) != 0U)
                ++stats.visible_result_drops;
        }
    }

    void prune_thermal_locked()
    {
        while (thermal_results.size() > config.max_thermal_results) {
            if (thermal_order.empty())
                break;
            const std::uint32_t sequence = thermal_order.front();
            thermal_order.pop_front();
            if (thermal_results.erase(sequence) != 0U)
                ++stats.thermal_result_drops;
        }
    }

    TemporalFusionJoinerConfig config;
    FrameSynchronizer synchronizer;
    mutable std::mutex mutex;
    std::unordered_map<std::uint32_t, VisibleFusionFrame> visible_results;
    std::unordered_map<std::uint32_t, ThermalFusionFrame> thermal_results;
    std::deque<std::uint32_t> visible_order;
    std::deque<std::uint32_t> thermal_order;
    TemporalFusionJoinerStats stats;
};

TemporalFusionJoiner::TemporalFusionJoiner(TemporalFusionJoinerConfig config)
    : impl_(new Impl(std::move(config)))
{
}

TemporalFusionJoiner::~TemporalFusionJoiner() = default;

std::vector<FusionInputPair> TemporalFusionJoiner::push_visible(
    VisibleFusionFrame frame)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::uint32_t sequence = frame.event.sequence;
    const auto existing = impl_->visible_results.find(sequence);
    if (existing == impl_->visible_results.end()) {
        impl_->visible_results.emplace(sequence, std::move(frame));
        impl_->visible_order.push_back(sequence);
    } else {
        existing->second = std::move(frame);
    }
    impl_->prune_visible_locked();
    impl_->synchronizer.push_visible(impl_->visible_results.at(sequence).event);
    return impl_->collect_locked();
}

std::vector<FusionInputPair> TemporalFusionJoiner::push_thermal(
    ThermalFusionFrame frame)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::uint32_t sequence = frame.event.sequence;
    const auto existing = impl_->thermal_results.find(sequence);
    if (existing == impl_->thermal_results.end()) {
        impl_->thermal_results.emplace(sequence, std::move(frame));
        impl_->thermal_order.push_back(sequence);
    } else {
        existing->second = std::move(frame);
    }
    impl_->prune_thermal_locked();
    impl_->synchronizer.push_thermal(impl_->thermal_results.at(sequence).event);
    return impl_->collect_locked();
}

std::vector<FusionInputPair> TemporalFusionJoiner::flush()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->synchronizer.flush();
    return impl_->collect_locked();
}

TemporalFusionJoinerStats TemporalFusionJoiner::stats() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    TemporalFusionJoinerStats output = impl_->stats;
    output.synchronizer = impl_->synchronizer.stats();
    output.visible_results_pending = impl_->visible_results.size();
    output.thermal_results_pending = impl_->thermal_results.size();
    return output;
}

}  // namespace p2
