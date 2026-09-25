// time_api.cpp — C ABI bridge from game .so to Clock time control
#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "core/clock.hpp"
#include "core/fixed_timestep.hpp"

extern "C" FJELL_SDK_EXPORT void fjell_time_set_scale(float scale) {
    auto* clock = fjell::script_services().clock;
    if (clock) clock->set_time_scale(scale);
}

extern "C" FJELL_SDK_EXPORT float fjell_time_get_scale() {
    auto* clock = fjell::script_services().clock;
    return clock ? clock->time_scale() : 1.0f;
}

extern "C" FJELL_SDK_EXPORT void fjell_time_set_paused(int paused) {
    auto* clock = fjell::script_services().clock;
    if (clock) clock->set_paused(paused != 0);
}

extern "C" FJELL_SDK_EXPORT int fjell_time_is_paused() {
    auto* clock = fjell::script_services().clock;
    return clock ? (clock->is_paused() ? 1 : 0) : 0;
}

extern "C" FJELL_SDK_EXPORT float fjell_time_fixed_delta() {
    const auto* timestep = fjell::script_services().fixed_timestep;
    return timestep ? static_cast<float>(timestep->step_seconds()) : 0.0f;
}
