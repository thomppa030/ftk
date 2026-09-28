// environment_api.hpp — Static time-of-day API for game scripts
#pragma once

#include "fjell/api.h"

#include "fjell/asset.hpp"

#include <glm/glm.hpp>

#include <cstdint>

/// The part of the day the sun's height puts the scene in. Twilight is the
/// band within six degrees of the horizon: dawn before noon, dusk after.
enum FjellTimePhase : int {
    FJELL_TIME_NIGHT = 0,
    FJELL_TIME_DAWN  = 1,
    FJELL_TIME_DAY   = 2,
    FJELL_TIME_DUSK  = 3,
};

/// Where a weather event is in its life. Approach: the front is coming, wind
/// rising, sky thickening from windward. Onset: the front passes overhead and
/// precipitation ramps. Sustain: the weather proper. Tail: it tapers and the
/// sky opens from the same side it closed from.
enum FjellWeatherPhase : int {
    FJELL_WEATHER_APPROACH = 0,
    FJELL_WEATHER_ONSET    = 1,
    FJELL_WEATHER_SUSTAIN  = 2,
    FJELL_WEATHER_TAIL     = 3,
};

/// What is falling, if anything.
enum FjellPrecipitation : int {
    FJELL_PRECIPITATION_NONE = 0,
    FJELL_PRECIPITATION_RAIN = 1,
    FJELL_PRECIPITATION_SNOW = 2,
};

extern "C" {
FJELL_SDK_EXPORT void  fjell_time_of_day_set_hour(float hour);
FJELL_SDK_EXPORT float fjell_time_of_day_hour();
FJELL_SDK_EXPORT void  fjell_time_of_day_set_day_length(float seconds);
FJELL_SDK_EXPORT float fjell_time_of_day_day_length();
FJELL_SDK_EXPORT int   fjell_time_of_day_phase();
FJELL_SDK_EXPORT float fjell_time_of_day_sun_elevation();
FJELL_SDK_EXPORT void  fjell_time_of_day_sun_direction(float* x, float* y, float* z);

FJELL_SDK_EXPORT void     fjell_weather_play(uint32_t event_handle_id);
FJELL_SDK_EXPORT void     fjell_weather_stop();
FJELL_SDK_EXPORT uint32_t fjell_weather_current();
FJELL_SDK_EXPORT int      fjell_weather_phase();
FJELL_SDK_EXPORT float    fjell_weather_phase_progress();
FJELL_SDK_EXPORT float    fjell_weather_precipitation();
FJELL_SDK_EXPORT int      fjell_weather_precipitation_type();
FJELL_SDK_EXPORT float    fjell_weather_wetness();
FJELL_SDK_EXPORT float    fjell_weather_snow();
FJELL_SDK_EXPORT float    fjell_weather_cloud_coverage();
FJELL_SDK_EXPORT void     fjell_weather_wind(float* x, float* y, float* z);
FJELL_SDK_EXPORT void     fjell_weather_front_direction(float* x, float* z);
FJELL_SDK_EXPORT float    fjell_weather_param(const char* name, float fallback);
FJELL_SDK_EXPORT int      fjell_weather_sky_param(const char* name, float* out, int count);
FJELL_SDK_EXPORT void     fjell_weather_set_sky_param(const char* name, int count, const float* values);
FJELL_SDK_EXPORT void     fjell_weather_set_climate(uint32_t climate_handle_id);
FJELL_SDK_EXPORT void     fjell_weather_hold(int held);
FJELL_SDK_EXPORT int      fjell_weather_is_held();
FJELL_SDK_EXPORT uint32_t fjell_weather_forecast(uint32_t index);
FJELL_SDK_EXPORT void     fjell_weather_strike(float x, float y, float z, float intensity);
}

namespace fjell {

/// A `.fjday` day profile, for `[[fj::property]] Asset<DayProfile>` fields.
struct DayProfile;
/// A `.fjweather` weather event, for `[[fj::property]] Asset<Weather>` fields.
struct Weather;
/// A `.fjclimate`, for `[[fj::property]] Asset<Climate>` fields.
struct Climate;

#ifndef FJELL_WEATHER_PHASE_DEFINED
#define FJELL_WEATHER_PHASE_DEFINED
enum class WeatherPhase : int { Approach = 0, Onset = 1, Sustain = 2, Tail = 3 };
#endif
enum class Precipitation : int { None = 0, Rain = 1, Snow = 2 };

/// Same values as the FjellTimePhase constants above. The engine declares
/// the same enum for its own use; the guard keeps them one type when both
/// headers reach the same translation unit.
#ifndef FJELL_TIME_PHASE_DEFINED
#define FJELL_TIME_PHASE_DEFINED
enum class TimePhase : int { Night = 0, Dawn = 1, Day = 2, Dusk = 3 };
#endif

/// The scene's clock and sun. The World Environment node owns the time of
/// day: its hour, how many real seconds a day takes, and the sun path the
/// hour moves the sun along. These calls read and write that node, so a
/// change shows in the editor and saves with the scene when made outside
/// play. A scene whose World Environment has no time of day enabled sits at
/// noon and the setters do nothing.
///
/// @code
/// void tick(float dt) override {
///     if (fjell::time_of_day::phase() == fjell::TimePhase::Night && !lantern_lit_) {
///         light_lantern();
///     }
/// }
/// @endcode
namespace time_of_day {
/// Jump the clock to an hour of the day.
/// @param hour 0..24; 6.5 is half past six in the morning. Values outside
///             the range wrap, so 25 is one in the morning.
inline void set_hour(float hour) { fjell_time_of_day_set_hour(hour); }

/// The current hour, 0..24.
[[nodiscard]] inline float hour() { return fjell_time_of_day_hour(); }

/// Set how long a full day takes.
/// @param seconds Real seconds per 24 hours, at time scale one. Zero
///                freezes the clock at the current hour.
inline void set_day_length(float seconds) { fjell_time_of_day_set_day_length(seconds); }

/// Real seconds per 24 hours; zero while the clock is frozen.
[[nodiscard]] inline float day_length() { return fjell_time_of_day_day_length(); }

/// Which part of the day it is, from the sun's height.
[[nodiscard]] inline TimePhase phase() {
    return static_cast<TimePhase>(fjell_time_of_day_phase());
}

/// Whether the sun is below the twilight band.
[[nodiscard]] inline bool is_night() { return phase() == TimePhase::Night; }

/// The sun's angle above the horizon in radians; negative once it has set.
[[nodiscard]] inline float sun_elevation() { return fjell_time_of_day_sun_elevation(); }

/// Unit direction from the ground toward the sun.
[[nodiscard]] inline glm::vec3 sun_direction() {
    glm::vec3 d;
    fjell_time_of_day_sun_direction(&d.x, &d.y, &d.z);
    return d;
}
} // namespace time_of_day

/// The weather over the scene. A weather is an event with a life of its own:
/// it approaches, sets in, holds and tails off, on the tracks its `.fjweather`
/// authored, and one event fades into the next by playing its tail under the
/// newcomer's approach. Scripts read what it has done to the world, wetness
/// and snow on surfaces, wind, precipitation, and can start an event.
///
/// @code
/// [[fj::property]] fjell::Asset<fjell::Weather> storm;
/// void on_trigger_enter(uint32_t) override { fjell::weather::play(storm); }
/// void tick(float) override {
///     footsteps_.set_wet(fjell::weather::wetness() > 0.5f);
/// }
/// @endcode
namespace weather {
/// Start an event now. Whatever is playing tails out underneath it.
/// @param event Event asset, normally a `[[fj::property]] Asset<Weather>`.
///              One that is unset or failed to load stops the game (see
///              `Asset`).
inline void play(const Asset<Weather>& event) {
    detail::require(event);
    fjell_weather_play(event.handle_id());
}

/// Let the current event tail out with nothing to follow: clear weather.
inline void stop() { fjell_weather_stop(); }

/// Handle id of the event playing, 0 when the sky is clear. Compare with
/// a ref's `handle_id()` to know which one it is.
[[nodiscard]] inline uint32_t current() { return fjell_weather_current(); }

/// Where the playing event is in its life.
[[nodiscard]] inline WeatherPhase phase() {
    return static_cast<WeatherPhase>(fjell_weather_phase());
}

/// 0..1 through the current phase.
[[nodiscard]] inline float phase_progress() { return fjell_weather_phase_progress(); }

/// How hard it is raining or snowing, 0..1.
[[nodiscard]] inline float precipitation() { return fjell_weather_precipitation(); }

/// What is falling.
[[nodiscard]] inline Precipitation precipitation_type() {
    return static_cast<Precipitation>(fjell_weather_precipitation_type());
}

/// How wet exposed surfaces are, 0..1. Rises while it rains and dries
/// off slowly afterwards.
[[nodiscard]] inline float wetness() { return fjell_weather_wetness(); }

/// How snow-covered exposed surfaces are, 0..1.
[[nodiscard]] inline float snow() { return fjell_weather_snow(); }

/// How much of the sky the clouds cover, 0..1.
[[nodiscard]] inline float cloud_coverage() { return fjell_weather_cloud_coverage(); }

/// The wind this frame: direction times speed in metres per second,
/// gusts included.
[[nodiscard]] inline glm::vec3 wind() {
    glm::vec3 w;
    fjell_weather_wind(&w.x, &w.y, &w.z);
    return w;
}

/// Which way the weather front travels over the ground, as a unit
/// direction on the XZ plane (x, z).
[[nodiscard]] inline glm::vec2 front_direction() {
    glm::vec2 d;
    fjell_weather_front_direction(&d.x, &d.y);
    return d;
}

/// A custom track of the playing event, by name: temperature, danger,
/// anything the designer keyed alongside the clouds.
/// @param fallback Returned when no playing event has the track.
[[nodiscard]] inline float param(const char* name, float fallback = 0.0f) {
    return fjell_weather_param(name, fallback);
}

/// A sky shader uniform as the environment has resolved it: the day
/// profile's track for this hour, with the playing weather blended over
/// it and any script override on top. The value the sky is actually
/// drawing with, not what any one of those asked for.
///
/// For anything that should agree with the sky rather than guess at it —
/// water reflecting it, a UI tint, a light keyed to the horizon. Reading
/// `sky_top` or `sky_bottom` is how a surface gets the sky's own colours
/// instead of a second palette that drifts from them.
///
/// Colours come back as sRGB, exactly as the picker and `.fjmat` show
/// them and exactly as `material::set_param` expects, so a colour can
/// be passed from one to the other untouched.
///
/// @param name     Track name: `sky_top`, `sky_bottom`, `fog_color`, or a
///                 custom one the day profile defines.
/// @param fallback Returned when nothing resolves that name.
///
/// @code
/// // Reflect the sky the sky is actually painting.
/// const glm::vec3 sky = fjell::weather::sky_color("sky_bottom");
/// fjell::material::set_param(sea_, "sky_color", sky.x, sky.y, sky.z);
/// @endcode
[[nodiscard]] inline glm::vec3 sky_color(const char* name,
                                         const glm::vec3& fallback = glm::vec3{0.0f}) {
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const int n = fjell_weather_sky_param(name, v, 4);
    return n >= 3 ? glm::vec3{v[0], v[1], v[2]} : fallback;
}

/// A single-float sky uniform as the environment resolved it — the same
/// lookup as `sky_color()` for tracks that are one number, such as
/// `star_intensity`.
[[nodiscard]] inline float sky_value(const char* name, float fallback = 0.0f) {
    float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    return fjell_weather_sky_param(name, v, 4) >= 1 ? v[0] : fallback;
}

/// Set a sky shader uniform by name, over the node's value and the day
/// profile's tracks. Colours as sRGB, as the picker shows them.
inline void set_sky_param(const char* name, float v) {
    fjell_weather_set_sky_param(name, 1, &v);
}
inline void set_sky_param(const char* name, float x, float y, float z) {
    const float v[3] = {x, y, z};
    fjell_weather_set_sky_param(name, 3, v);
}
inline void set_sky_param(const char* name, float x, float y, float z, float w) {
    const float v[4] = {x, y, z, w};
    fjell_weather_set_sky_param(name, 4, v);
}
/// Hand a uniform back to the node and profile.
inline void clear_sky_param(const char* name) { fjell_weather_set_sky_param(name, 0, nullptr); }

/// Hand the weather to a climate: it rolls what comes next when an event
/// ends, so the sky keeps having weather without a script deciding each
/// turn. A climate that is unset or failed to load stops the game (see
/// `Asset`); `clear_climate()` is how to have none.
inline void set_climate(const Asset<Climate>& climate) {
    detail::require(climate);
    fjell_weather_set_climate(climate.handle_id());
}

/// Take the weather away from its climate: whatever plays runs out, and
/// nothing is rolled after it.
inline void clear_climate() { fjell_weather_set_climate(0); }

/// Freeze the climate: whatever plays keeps playing and nothing new is
/// rolled. `play()` still works, for weather a script drives itself.
inline void hold(bool held = true) { fjell_weather_hold(held ? 1 : 0); }
[[nodiscard]] inline bool is_held() { return fjell_weather_is_held() != 0; }

/// What the climate will play after this, soonest first: the handle id
/// of that event, 0 past the end of the forecast or without a climate.
/// Compare it with an `Asset<Weather>`'s `handle_id()` to see a storm
/// coming and act before it arrives.
/// @param index 0 is the next event, 1 the one after it.
[[nodiscard]] inline uint32_t forecast(uint32_t index = 0) {
    return fjell_weather_forecast(index);
}

/// Strike lightning now, without waiting for the storm to roll one: the
/// playing event's lightning, so its colour, reach and flash are the
/// ones the artist authored.
/// @param offset    Where it lands relative to the listener, in metres.
/// @param intensity Scales the flash and the light.
inline void strike(const glm::vec3& offset, float intensity = 1.0f) {
    fjell_weather_strike(offset.x, offset.y, offset.z, intensity);
}
} // namespace weather

} // namespace fjell
