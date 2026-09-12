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

/// Handle to a scheduled timer, returned by `Timer::set()` and
/// `Timer::set_repeating()`. Store it if you may need to cancel; discard it
/// for a fire-and-forget one-shot.
///
/// Safe against use-after-cancel: the handle carries a generation, so
/// cancelling an already-fired or already-cancelled timer is a no-op rather
/// than hitting an unrelated timer that reused the slot.
struct TimerHandle {
    uint64_t packed{0};

    /// Whether this handle was ever assigned a timer. Does not report whether
    /// that timer is still pending.
    [[nodiscard]] bool valid() const { return packed != 0; }
};

/// Schedules callbacks on the gameplay clock. Timers respect `Time::set_scale()`
/// and stop advancing while `Time::pause()` is active.
///
/// @warning Timers are NOT cancelled when the owning component is destroyed —
///          only when play stops or scripts hot-reload. A timer bound to
///          `this` will fire into a freed object if the node is destroyed
///          first. Store the handle and cancel it in `end_play()`.
///
/// @code
/// class Turret : public fjell::ScriptComponent {
///     FJELL_COMPONENT(Turret)
///     fjell::TimerHandle fire_timer_;
/// public:
///     void begin_play() override {
///         fire_timer_ = fjell::Timer::set_repeating<&Turret::fire>(1.5f, this);
///     }
///     void end_play() override { fjell::Timer::cancel(fire_timer_); }
///     void fire() { /* ... */ }
/// };
/// @endcode
struct Timer {
    /// Schedule a one-shot callback.
    /// @tparam Method  Pointer to a zero-argument member function, passed as a
    ///                 template argument: `Timer::set<&MyClass::on_timeout>(...)`.
    /// @param delay    Seconds to wait, in scaled gameplay time.
    /// @param instance Object to call the method on — normally `this`. It must
    ///                 outlive the delay, or the timer must be cancelled first.
    /// @return Handle for cancellation. Keep it only if you may cancel.
    template <auto Method, typename T>
    static TimerHandle set(float delay, T* instance) {
        return {fjell_timer_set(delay, instance,
            [](void* self) { (static_cast<T*>(self)->*Method)(); })};
    }

    /// Schedule a callback that repeats until cancelled.
    /// @tparam Method  Pointer to a zero-argument member function.
    /// @param interval Seconds between firings, in scaled gameplay time. The
    ///                 first call happens after one full interval, not immediately.
    /// @param instance Object to call the method on — normally `this`.
    /// @return Handle for cancellation. Always store this one: a repeating
    ///         timer runs until play stops otherwise.
    template <auto Method, typename T>
    static TimerHandle set_repeating(float interval, T* instance) {
        return {fjell_timer_set_repeating(interval, instance,
            [](void* self) { (static_cast<T*>(self)->*Method)(); })};
    }

    /// Cancel a pending timer. Safe to call with a default-constructed handle,
    /// an already-fired one-shot, or a handle cancelled earlier — all no-ops.
    static void cancel(TimerHandle handle) {
        if (handle.valid()) fjell_timer_cancel(handle.packed);
    }
};

} // namespace fjell
