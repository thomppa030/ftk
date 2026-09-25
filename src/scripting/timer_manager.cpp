// timer_manager.cpp — Engine-side timer management
#include "scripting/timer_manager.hpp"

#include <algorithm>

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
    for (auto& e : entries_) {
        if (e.generation == 0) continue;

        e.remaining -= dt;
        if (e.remaining <= 0.0f) {
            // Fire callback
            if (e.callback) {
                e.callback();
            }

            if (e.interval > 0.0f) {
                // Repeating — reset with leftover time
                e.remaining += e.interval;
                if (e.remaining < 0.0f) e.remaining = 0.0f;
            } else {
                // One-shot — mark as free
                e.generation = 0;
                e.callback = nullptr;
            }
        }
    }
}

void TimerManager::clear() {
    entries_.clear();
}

} // namespace fjell
