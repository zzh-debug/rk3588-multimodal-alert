#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <variant>

#include "p2/capture/frame_types.hpp"

namespace p2 {

using FrameEvent = std::variant<VisibleFrameEvent, ThermalFrameEvent>;

struct EventQueueStats {
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    std::uint64_t visible_drops = 0;
    std::uint64_t thermal_drops = 0;
    std::size_t high_watermark = 0;
    std::size_t pending = 0;
};

class BoundedEventQueue {
public:
    explicit BoundedEventQueue(std::size_t capacity);

    bool push(FrameEvent event);
    bool wait_pop(FrameEvent *event);
    void close();

    EventQueueStats stats() const;
    std::size_t capacity() const { return capacity_; }

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<FrameEvent> events_;
    EventQueueStats stats_;
    bool closed_ = false;
};

}  // namespace p2
