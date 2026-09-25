// timer_api.cpp — C ABI bridge from game .so to engine TimerManager
#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "scripting/timer_manager.hpp"

namespace {

/// Pack engine-side TimerHandle (index + generation) into a uint64_t for the SDK.
uint64_t pack(fjell::TimerHandle h) {
    return (static_cast<uint64_t>(h.generation) << 32) | h.index;
}

/// Unpack SDK uint64_t back to engine-side TimerHandle.
fjell::TimerHandle unpack(uint64_t packed) {
    return {static_cast<uint32_t>(packed & 0xFFFFFFFF),
            static_cast<uint32_t>(packed >> 32)};
}

} // anonymous namespace

extern "C" FJELL_SDK_EXPORT uint64_t fjell_timer_set(float delay, void* instance,
                                                     void (*fn)(void*)) {
    auto* mgr = fjell::script_services().timers;
    if (!mgr) return 0;
    auto handle = mgr->set(delay, [fn, instance]() { fn(instance); }, instance);
    return pack(handle);
}

extern "C" FJELL_SDK_EXPORT uint64_t fjell_timer_set_repeating(float interval,
                                                               void* instance,
                                                               void (*fn)(void*)) {
    auto* mgr = fjell::script_services().timers;
    if (!mgr) return 0;
    auto handle = mgr->set_repeating(interval, [fn, instance]() { fn(instance); }, instance);
    return pack(handle);
}

extern "C" FJELL_SDK_EXPORT void fjell_timer_cancel(uint64_t packed_handle) {
    auto* mgr = fjell::script_services().timers;
    if (!mgr) return;
    mgr->cancel(unpack(packed_handle));
}
