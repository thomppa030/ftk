// timer_api.hpp — Static timer API for game scripts
#pragma once

#include "fjell/api.h"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT uint64_t fjell_timer_set(float delay, void* instance,
                                          void (*fn)(void*));
FJELL_SDK_EXPORT uint64_t fjell_timer_set_repeating(float interval, void* instance,
                                                    void (*fn)(void*));
FJELL_SDK_EXPORT void fjell_timer_cancel(uint64_t packed_handle);
}

namespace fjell {

/// Opaque timer handle returned by Timer::set / Timer::set_repeating.
struct TimerHandle {
    uint64_t packed{0};

    [[nodiscard]] bool valid() const { return packed != 0; }
};

struct Timer {
    /// Schedule a one-shot callback after `delay` seconds.
    /// Usage: Timer::set<&MyClass::on_timeout>(0.5f, this);
    template <auto Method, typename T>
    static TimerHandle set(float delay, T* instance) {
        return {fjell_timer_set(delay, instance,
            [](void* self) { (static_cast<T*>(self)->*Method)(); })};
    }

    /// Schedule a repeating callback every `interval` seconds.
    /// Usage: Timer::set_repeating<&MyClass::on_tick>(1.0f, this);
    template <auto Method, typename T>
    static TimerHandle set_repeating(float interval, T* instance) {
        return {fjell_timer_set_repeating(interval, instance,
            [](void* self) { (static_cast<T*>(self)->*Method)(); })};
    }

    /// Cancel a timer. Safe to call with invalid/expired handles.
    static void cancel(TimerHandle handle) {
        if (handle.valid()) fjell_timer_cancel(handle.packed);
    }
};

} // namespace fjell
