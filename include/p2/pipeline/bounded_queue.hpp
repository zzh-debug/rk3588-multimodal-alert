#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace p2 {

struct BoundedQueueStats {
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    std::uint64_t dropped_oldest = 0;
    std::uint64_t shutdown_drops = 0;
    std::uint64_t closed_rejections = 0;
    std::size_t high_watermark = 0;
    std::size_t pending = 0;
};

template <typename T>
class BoundedLatestQueue {
public:
    explicit BoundedLatestQueue(std::size_t capacity)
        : capacity_(capacity)
    {
        if (capacity_ == 0)
            throw std::invalid_argument("bounded queue capacity must be positive");
    }

    BoundedLatestQueue(const BoundedLatestQueue &) = delete;
    BoundedLatestQueue &operator=(const BoundedLatestQueue &) = delete;

    bool push(T item)
    {
        std::optional<T> dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                ++stats_.closed_rejections;
                return false;
            }
            ++stats_.pushed;
            if (queue_.size() == capacity_) {
                dropped.emplace(std::move(queue_.front()));
                queue_.pop_front();
                ++stats_.dropped_oldest;
            }
            queue_.push_back(std::move(item));
            if (queue_.size() > stats_.high_watermark)
                stats_.high_watermark = queue_.size();
        }
        condition_.notify_one();
        return true;
    }

    bool wait_pop(T *item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() {
            return closed_ || !queue_.empty();
        });
        if (queue_.empty())
            return false;
        *item = std::move(queue_.front());
        queue_.pop_front();
        ++stats_.popped;
        return true;
    }

    void close(bool discard_pending)
    {
        std::deque<T> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_)
                return;
            closed_ = true;
            if (discard_pending) {
                stats_.shutdown_drops += queue_.size();
                discarded.swap(queue_);
            }
        }
        condition_.notify_all();
    }

    BoundedQueueStats stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        BoundedQueueStats snapshot = stats_;
        snapshot.pending = queue_.size();
        return snapshot;
    }

    std::size_t capacity() const { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<T> queue_;
    bool closed_ = false;
    BoundedQueueStats stats_;
};

}  // namespace p2
