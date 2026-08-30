#include "p2/sync/frame_synchronizer.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace p2 {
namespace {

std::uint64_t absolute_difference(std::uint64_t lhs, std::uint64_t rhs)
{
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

std::int64_t signed_difference(std::uint64_t lhs, std::uint64_t rhs)
{
    if (lhs >= rhs) {
        const auto difference = lhs - rhs;
        if (difference > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max()))
            return std::numeric_limits<std::int64_t>::max();
        return static_cast<std::int64_t>(difference);
    }
    const auto difference = rhs - lhs;
    if (difference > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(difference);
}

}  // namespace

FrameSynchronizer::FrameSynchronizer(SynchronizerConfig config)
    : config_(config)
{
    if (config_.max_visible_history == 0 ||
        config_.max_pending_thermal == 0)
        throw std::invalid_argument("synchronizer queue size must be positive");
}

void FrameSynchronizer::push_visible(const VisibleFrameEvent &frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.visible_received;
    if (!visible_.empty() && frame.timestamp_ns < visible_.back().timestamp_ns) {
        ++stats_.visible_out_of_order;
        const auto position = std::upper_bound(
            visible_.begin(), visible_.end(), frame.timestamp_ns,
            [](std::uint64_t timestamp, const VisibleFrameEvent &candidate) {
                return timestamp < candidate.timestamp_ns;
            });
        visible_.insert(position, frame);
    } else {
        visible_.push_back(frame);
    }
    stats_.visible_high_watermark =
        std::max(stats_.visible_high_watermark, visible_.size());
    prune_visible_locked();
    process_pending_locked(false);
}

void FrameSynchronizer::push_thermal(const ThermalFrameEvent &frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.thermal_received;
    if (!thermal_.empty() && frame.timestamp_ns < thermal_.back().timestamp_ns) {
        ++stats_.thermal_out_of_order;
        const auto position = std::upper_bound(
            thermal_.begin(), thermal_.end(), frame.timestamp_ns,
            [](std::uint64_t timestamp, const ThermalFrameEvent &candidate) {
                return timestamp < candidate.timestamp_ns;
            });
        thermal_.insert(position, frame);
    } else {
        thermal_.push_back(frame);
    }
    if (thermal_.size() > config_.max_pending_thermal) {
        thermal_.pop_front();
        ++stats_.thermal_queue_drops;
    }
    stats_.thermal_high_watermark =
        std::max(stats_.thermal_high_watermark, thermal_.size());
    process_pending_locked(false);
}

void FrameSynchronizer::flush()
{
    std::lock_guard<std::mutex> lock(mutex_);
    process_pending_locked(true);
}

SynchronizerStats FrameSynchronizer::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    SynchronizerStats result = stats_;
    result.visible_pending = visible_.size();
    result.thermal_pending = thermal_.size();
    return result;
}

std::vector<MatchRecord> FrameSynchronizer::matches() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return matches_;
}

std::vector<MatchRecord> FrameSynchronizer::drain_matches()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MatchRecord> result;
    result.swap(matches_);
    return result;
}

void FrameSynchronizer::process_pending_locked(bool force)
{
    while (!thermal_.empty()) {
        const ThermalFrameEvent &thermal = thermal_.front();
        if (!force) {
            if (visible_.empty())
                return;
            const std::uint64_t ready_at =
                thermal.timestamp_ns + config_.lookahead_ns;
            if (visible_.back().timestamp_ns < ready_at)
                return;
        }

        if (visible_.empty()) {
            ++stats_.unmatched_no_visible;
            thermal_.pop_front();
            continue;
        }

        auto best = visible_.begin();
        std::uint64_t best_difference =
            absolute_difference(best->timestamp_ns, thermal.timestamp_ns);
        for (auto candidate = std::next(visible_.begin());
             candidate != visible_.end(); ++candidate) {
            const std::uint64_t difference = absolute_difference(
                candidate->timestamp_ns, thermal.timestamp_ns);
            if (difference < best_difference) {
                best = candidate;
                best_difference = difference;
            }
        }

        if (config_.max_skew_ns != 0 &&
            best_difference > config_.max_skew_ns) {
            ++stats_.unmatched_skew;
        } else {
            MatchRecord record;
            record.visible_sequence = best->sequence;
            record.thermal_sequence = thermal.sequence;
            record.thermal_pair_sequence = thermal.pair_sequence;
            record.visible_timestamp_ns = best->timestamp_ns;
            record.thermal_timestamp_ns = thermal.timestamp_ns;
            record.thermal_span_ns =
                thermal.second_ready_ns - thermal.first_ready_ns;
            record.visible_arrival_ns = best->arrival_ns;
            record.thermal_arrival_ns = thermal.arrival_ns;
            record.signed_delta_ns = signed_difference(
                best->timestamp_ns, thermal.timestamp_ns);
            record.absolute_delta_ns = best_difference;
            matches_.push_back(record);
            ++stats_.matched;
        }
        thermal_.pop_front();
        prune_visible_locked();
    }
}

void FrameSynchronizer::prune_visible_locked()
{
    while (visible_.size() > config_.max_visible_history) {
        visible_.pop_front();
        ++stats_.visible_history_pruned;
    }
}

}  // namespace p2
