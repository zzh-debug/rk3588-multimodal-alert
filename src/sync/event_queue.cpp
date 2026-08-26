#include "p2/sync/event_queue.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace p2 {

BoundedEventQueue::BoundedEventQueue(std::size_t capacity)
    : capacity_(capacity)
{
    if (capacity_ == 0)
        throw std::invalid_argument("event queue capacity must be positive");
}

bool BoundedEventQueue::push(FrameEvent event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
        return false;
    if (events_.size() >= capacity_) {
        if (std::holds_alternative<VisibleFrameEvent>(events_.front()))
            ++stats_.visible_drops;
        else
            ++stats_.thermal_drops;
        events_.pop_front();
    }
    events_.push_back(std::move(event));
    ++stats_.pushed;
    stats_.high_watermark = std::max(stats_.high_watermark, events_.size());
    condition_.notify_one();
    return true;
}

bool BoundedEventQueue::wait_pop(FrameEvent *event)
{
    if (event == nullptr)
        return false;
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&]() { return closed_ || !events_.empty(); });
    if (events_.empty())
        return false;
    *event = std::move(events_.front());
    events_.pop_front();
    ++stats_.popped;
    return true;
}

void BoundedEventQueue::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    condition_.notify_all();
}

EventQueueStats BoundedEventQueue::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    EventQueueStats result = stats_;
    result.pending = events_.size();
    return result;
}

}  // namespace p2
