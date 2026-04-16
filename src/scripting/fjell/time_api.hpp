// time_api.hpp — Static time control API for game scripts
#pragma once

#include "fjell/api.h"

extern "C" {
FJELL_SDK_EXPORT void  fjell_time_set_scale(float scale);
FJELL_SDK_EXPORT float fjell_time_get_scale();
FJELL_SDK_EXPORT void  fjell_time_set_paused(int paused);
FJELL_SDK_EXPORT int   fjell_time_is_paused();
}

namespace fjell {

struct Time {
    /// Set gameplay time scale (1.0 = normal, 0.5 = half speed, 2.0 = double).
    static void set_scale(float scale) { fjell_time_set_scale(scale); }
    [[nodiscard]] static float scale() { return fjell_time_get_scale(); }

    /// Pause all gameplay (scripts, VFX, timers). Rendering continues.
    static void pause() { fjell_time_set_paused(1); }
    static void resume() { fjell_time_set_paused(0); }
    [[nodiscard]] static bool is_paused() { return fjell_time_is_paused() != 0; }
};

} // namespace fjell
