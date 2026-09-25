// timer_manager.hpp — Engine-side timer management for game scripts
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace fjell {

/// Opaque handle for a timer. The generation prevents use-after-cancel.
struct TimerHandle {
    uint32_t index{0};
    uint32_t generation{0};

    [[nodiscard]] bool valid() const { return generation != 0; }
    bool operator==(const TimerHandle&) const = default;
};

class TimerManager {
public:
    using Callback = std::function<void()>;

    /// Create a one-shot timer. Returns a handle for cancellation. `owner` is
    /// what the callback calls into (a script instance), so every timer it set
    /// can be cancelled when it goes; null for a timer nothing owns.
    TimerHandle set(float delay, Callback callback, const void* owner = nullptr);

    /// Create a repeating timer. Fires every `interval` seconds.
    TimerHandle set_repeating(float interval, Callback callback, const void* owner = nullptr);

    /// Cancel a timer. Safe to call with invalid/expired handles.
    void cancel(TimerHandle handle);

    /// Cancel every timer `owner` set.
    void cancel_owned(const void* owner);

    /// Advance all timers by dt, fire expired ones.
    void tick(float dt);

    /// Cancel all timers (called on play stop / hot-reload).
    void clear();

private:
    struct Entry {
        Callback callback;
        float remaining{0.0f};
        float interval{0.0f};   // >0 = repeating
        uint32_t generation{0}; // 0 = free slot
        const void* owner{nullptr};
    };

    std::vector<Entry> entries_;
    uint32_t next_generation_{1};
};

} // namespace fjell
