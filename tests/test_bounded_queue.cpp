#include "p2/pipeline/bounded_queue.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

}  // namespace

int main()
{
    try {
        p2::BoundedLatestQueue<std::unique_ptr<int>> queue(2);
        require(queue.push(std::make_unique<int>(1)), "push 1 failed");
        require(queue.push(std::make_unique<int>(2)), "push 2 failed");
        require(queue.push(std::make_unique<int>(3)), "push 3 failed");

        std::unique_ptr<int> value;
        require(queue.wait_pop(&value) && *value == 2,
                "drop-oldest did not preserve the newest values");
        require(queue.wait_pop(&value) && *value == 3,
                "queue order changed");

        require(queue.push(std::make_unique<int>(4)), "push 4 failed");
        queue.close(true);
        require(!queue.wait_pop(&value), "closed discarded queue returned data");
        require(!queue.push(std::make_unique<int>(5)),
                "closed queue accepted data");

        const p2::BoundedQueueStats stats = queue.stats();
        require(stats.pushed == 4, "unexpected pushed count");
        require(stats.popped == 2, "unexpected popped count");
        require(stats.dropped_oldest == 1,
                "unexpected controlled-drop count");
        require(stats.shutdown_drops == 1,
                "unexpected shutdown-drop count");
        require(stats.closed_rejections == 1,
                "unexpected closed-rejection count");
        require(stats.high_watermark == 2, "unexpected high-watermark");
        require(stats.pending == 0, "queue did not drain on close");
        std::cout << "bounded queue tests passed\n";
    } catch (const std::exception &exception) {
        std::cerr << "bounded queue test failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
