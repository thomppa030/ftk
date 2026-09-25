// time_api.hpp — Static time control API for game scripts
#pragma once

#include "fjell/api.h"

extern "C" {
FJELL_SDK_EXPORT void  fjell_time_set_scale(float scale);
FJELL_SDK_EXPORT float fjell_time_get_scale();
FJELL_SDK_EXPORT void  fjell_time_set_paused(int paused);
FJELL_SDK_EXPORT int   fjell_time_is_paused();
FJELL_SDK_EXPORT float fjell_time_fixed_delta();
}

namespace fjell {

/// Global gameplay clock control. Affects script ticks, timers, animation, and
/// VFX; rendering and the editor UI keep running at full rate.
struct Time {
    /// Scale the passage of gameplay time.
    /// @param scale 1.0 normal, 0.5 half speed, 2.0 double, 0.0 frozen.
    ///              Feeds straight into every `tick(dt)` and timer.
    static void set_scale(float scale) { fjell_time_set_scale(scale); }

    /// The current time scale.
    [[nodiscard]] static float scale() { return fjell_time_get_scale(); }

    /// Pause gameplay — scripts, timers, animation, and VFX all stop.
    /// Rendering continues, so a paused frame still draws and a menu built on
    /// the UI system stays interactive.
    static void pause() { fjell_time_set_paused(1); }

    /// Resume from `pause()`. The time scale is unchanged by pausing.
    static void resume() { fjell_time_set_paused(0); }

    /// Whether gameplay is currently paused.
    [[nodiscard]] static bool is_paused() { return fjell_time_is_paused() != 0; }

    /// The simulation's step length in seconds: what `fixed_tick()` receives
    /// as `step.dt`, the same for every step of a session.
    [[nodiscard]] static float fixed_delta() { return fjell_time_fixed_delta(); }
};

} // namespace fjell
