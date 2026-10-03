#include "ftk/gpu/release_queue.hpp"

#include <algorithm>

namespace ftk::gpu {

void ReleaseQueue::defer(uint64_t after, std::move_only_function<void()> fn) {
    entries_.push_back({after, std::move(fn)});
}

void ReleaseQueue::collect(uint64_t completed) {
    // Taken off the queue before it runs: a destructor may retire more, which
    // lands behind it.
    while (!entries_.empty() && entries_.front().after <= completed) {
        auto fn = std::move(entries_.front().fn);
        entries_.pop_front();
        fn();
    }
    collected_ = std::max(collected_, completed);
}

void ReleaseQueue::flush() {
    while (!entries_.empty()) {
        auto fn = std::move(entries_.front().fn);
        entries_.pop_front();
        fn();
    }
}

} // namespace ftk::gpu
