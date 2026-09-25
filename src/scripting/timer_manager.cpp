// timer_manager.cpp — Engine-side timer management
#include "scripting/timer_manager.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace fjell {

TimerHandle TimerManager::set(float delay, Callback callback, const void* owner) {
    // Find a free slot (generation == 0)
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].generation == 0) {
            uint32_t gen = next_generation_++;
            entries_[i] = {std::move(callback), delay, 0.0f, gen, owner};
            return {i, gen};
        }
    }

    // No free slot — append
    uint32_t idx = static_cast<uint32_t>(entries_.size());
    uint32_t gen = next_generation_++;
    entries_.push_back({std::move(callback), delay, 0.0f, gen, owner});
    return {idx, gen};
}

TimerHandle TimerManager::set_repeating(float interval, Callback callback, const void* owner) {
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].generation == 0) {
            uint32_t gen = next_generation_++;
            entries_[i] = {std::move(callback), interval, interval, gen, owner};
            return {i, gen};
        }
    }

    uint32_t idx = static_cast<uint32_t>(entries_.size());
    uint32_t gen = next_generation_++;
    entries_.push_back({std::move(callback), interval, interval, gen, owner});
    return {idx, gen};
}

void TimerManager::cancel(TimerHandle handle) {
    if (handle.index >= entries_.size()) return;
    auto& e = entries_[handle.index];
    if (e.generation != handle.generation) return;
    e.generation = 0;
    e.callback = nullptr;
}

void TimerManager::cancel_owned(const void* owner) {
    if (owner == nullptr) return;
    for (auto& e : entries_) {
        if (e.generation == 0 || e.owner != owner) continue;
        e.generation = 0;
        e.callback = nullptr;
        e.owner = nullptr;
    }
}

void TimerManager::tick(float dt) {
    // By index and over the timers there were when the tick began: a callback
    // may set a timer, which can grow entries_ under a reference, or cancel
    // one, its own included. A timer set now first counts down next tick.
    const std::size_t count = entries_.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (entries_[i].generation == 0) continue;

        entries_[i].remaining -= dt;
        if (entries_[i].remaining > 0.0f) continue;

        // The callback is moved out while it runs, so the slot can change
        // under it without the function it is running going away.
        const uint32_t generation = entries_[i].generation;
        Callback callback = std::move(entries_[i].callback);
        if (callback) callback();

        Entry& e = entries_[i];
        if (e.generation != generation) continue;   // cancelled from its own callback
        if (e.interval > 0.0f) {
            // Repeating — reset with leftover time
            e.callback = std::move(callback);
            e.remaining += e.interval;
            if (e.remaining < 0.0f) e.remaining = 0.0f;
        } else {
            // One-shot — mark as free
            e.generation = 0;
            e.owner = nullptr;
        }
    }
}

void TimerManager::clear() {
    entries_.clear();
}

} // namespace fjell
