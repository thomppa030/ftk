// environment_api.hpp — Static time-of-day API for game scripts
#pragma once

#include "fjell/api.h"

#include <glm/glm.hpp>

/// The part of the day the sun's height puts the scene in. Twilight is the
/// band within six degrees of the horizon: dawn before noon, dusk after.
enum FjellTimePhase : int {
    FJELL_TIME_NIGHT = 0,
    FJELL_TIME_DAWN  = 1,
    FJELL_TIME_DAY   = 2,
    FJELL_TIME_DUSK  = 3,
};

extern "C" {
FJELL_SDK_EXPORT void  fjell_time_of_day_set_hour(float hour);
FJELL_SDK_EXPORT float fjell_time_of_day_hour();
FJELL_SDK_EXPORT void  fjell_time_of_day_set_day_length(float seconds);
FJELL_SDK_EXPORT float fjell_time_of_day_day_length();
FJELL_SDK_EXPORT int   fjell_time_of_day_phase();
FJELL_SDK_EXPORT float fjell_time_of_day_sun_elevation();
FJELL_SDK_EXPORT void  fjell_time_of_day_sun_direction(float* x, float* y, float* z);
}

namespace fjell {

/// A `.fjday` day profile, for `[[fj::property]] Ref<DayProfileAsset>` fields.
struct DayProfileAsset;

/// Same values as the FjellTimePhase constants above.
enum class TimePhase : int { Night = 0, Dawn = 1, Day = 2, Dusk = 3 };

/// The scene's clock and sun. The World Environment node owns the time of
/// day: its hour, how many real seconds a day takes, and the sun path the
/// hour moves the sun along. These calls read and write that node, so a
/// change shows in the editor and saves with the scene when made outside
/// play. A scene whose World Environment has no time of day enabled sits at
/// noon and the setters do nothing.
///
/// @code
/// void tick(float dt) override {
///     if (fjell::TimeOfDay::phase() == fjell::TimePhase::Night && !lantern_lit_) {
///         light_lantern();
///     }
/// }
/// @endcode
struct TimeOfDay {
    /// Jump the clock to an hour of the day.
    /// @param hour 0..24; 6.5 is half past six in the morning. Values outside
    ///             the range wrap, so 25 is one in the morning.
    static void set_hour(float hour) { fjell_time_of_day_set_hour(hour); }

    /// The current hour, 0..24.
    [[nodiscard]] static float hour() { return fjell_time_of_day_hour(); }

    /// Set how long a full day takes.
    /// @param seconds Real seconds per 24 hours, at time scale one. Zero
    ///                freezes the clock at the current hour.
    static void set_day_length(float seconds) { fjell_time_of_day_set_day_length(seconds); }

    /// Real seconds per 24 hours; zero while the clock is frozen.
    [[nodiscard]] static float day_length() { return fjell_time_of_day_day_length(); }

    /// Which part of the day it is, from the sun's height.
    [[nodiscard]] static TimePhase phase() {
        return static_cast<TimePhase>(fjell_time_of_day_phase());
    }

    /// Whether the sun is below the twilight band.
    [[nodiscard]] static bool is_night() { return phase() == TimePhase::Night; }

    /// The sun's angle above the horizon in radians; negative once it has set.
    [[nodiscard]] static float sun_elevation() { return fjell_time_of_day_sun_elevation(); }

    /// Unit direction from the ground toward the sun.
    [[nodiscard]] static glm::vec3 sun_direction() {
        glm::vec3 d;
        fjell_time_of_day_sun_direction(&d.x, &d.y, &d.z);
        return d;
    }
};

} // namespace fjell
