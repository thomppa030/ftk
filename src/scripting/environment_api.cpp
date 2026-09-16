// environment_api.cpp — C ABI bridge from game .so to the environment subsystem
#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "environment/environment_subsystem.hpp"
#include "renderer/resources/resource_manager.hpp"

#include <algorithm>

extern "C" FJELL_SDK_EXPORT void fjell_time_of_day_set_hour(float hour) {
    auto* env = fjell::script_services().environment;
    if (env) env->set_hour(hour);
}

extern "C" FJELL_SDK_EXPORT float fjell_time_of_day_hour() {
    auto* env = fjell::script_services().environment;
    return env ? env->hour() : 12.0f;
}

extern "C" FJELL_SDK_EXPORT void fjell_time_of_day_set_day_length(float seconds) {
    auto* env = fjell::script_services().environment;
    if (env) env->set_day_length(seconds);
}

extern "C" FJELL_SDK_EXPORT float fjell_time_of_day_day_length() {
    auto* env = fjell::script_services().environment;
    return env ? env->day_length() : 0.0f;
}

extern "C" FJELL_SDK_EXPORT int fjell_time_of_day_phase() {
    auto* env = fjell::script_services().environment;
    return env ? static_cast<int>(env->state().phase) : static_cast<int>(fjell::TimePhase::Day);
}

extern "C" FJELL_SDK_EXPORT float fjell_time_of_day_sun_elevation() {
    auto* env = fjell::script_services().environment;
    return env ? env->state().sun.elevation : 0.0f;
}

extern "C" FJELL_SDK_EXPORT void fjell_time_of_day_sun_direction(float* x, float* y, float* z) {
    auto* env = fjell::script_services().environment;
    const glm::vec3 d = env ? env->state().sun.direction : glm::vec3{0.0f, 1.0f, 0.0f};
    if (x) *x = d.x;
    if (y) *y = d.y;
    if (z) *z = d.z;
}

extern "C" FJELL_SDK_EXPORT void fjell_weather_play(uint32_t event_handle_id) {
    auto* env = fjell::script_services().environment;
    if (env) env->play_weather(fjell::WeatherAssetHandle{event_handle_id});
}

extern "C" FJELL_SDK_EXPORT void fjell_weather_stop() {
    auto* env = fjell::script_services().environment;
    if (env) env->play_weather(fjell::WeatherAssetHandle{});
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_weather_current() {
    auto* env = fjell::script_services().environment;
    return env ? env->state().weather.event_id : 0u;
}

extern "C" FJELL_SDK_EXPORT int fjell_weather_phase() {
    auto* env = fjell::script_services().environment;
    return env ? static_cast<int>(env->state().weather.phase) : static_cast<int>(fjell::WeatherPhase::Sustain);
}

extern "C" FJELL_SDK_EXPORT float fjell_weather_phase_progress() {
    auto* env = fjell::script_services().environment;
    return env ? env->state().weather.phase_progress : 1.0f;
}

extern "C" FJELL_SDK_EXPORT float fjell_weather_precipitation() {
    auto* env = fjell::script_services().environment;
    return env ? env->state().weather.precipitation : 0.0f;
}

extern "C" FJELL_SDK_EXPORT int fjell_weather_precipitation_type() {
    auto* env = fjell::script_services().environment;
    return env ? static_cast<int>(env->state().weather.precipitation_type) : 0;
}

extern "C" FJELL_SDK_EXPORT float fjell_weather_wetness() {
    auto* env = fjell::script_services().environment;
    return env ? env->state().weather.wetness : 0.0f;
}

extern "C" FJELL_SDK_EXPORT float fjell_weather_snow() {
    auto* env = fjell::script_services().environment;
    return env ? env->state().weather.snow : 0.0f;
}

extern "C" FJELL_SDK_EXPORT float fjell_weather_cloud_coverage() {
    auto* env = fjell::script_services().environment;
    return env ? env->state().weather.cloud_coverage : 0.0f;
}

extern "C" FJELL_SDK_EXPORT void fjell_weather_wind(float* x, float* y, float* z) {
    auto* env = fjell::script_services().environment;
    glm::vec3 w{0.0f};
    if (env) {
        const auto& p = env->state().weather;
        w = p.wind_direction * (p.wind_speed * p.wind_gust);
    }
    if (x) *x = w.x;
    if (y) *y = w.y;
    if (z) *z = w.z;
}

extern "C" FJELL_SDK_EXPORT void fjell_weather_front_direction(float* x, float* z) {
    auto* env = fjell::script_services().environment;
    const glm::vec2 d = env ? env->state().weather.front_direction : glm::vec2{0.0f, 1.0f};
    if (x) *x = d.x;
    if (z) *z = d.y;
}

extern "C" FJELL_SDK_EXPORT float fjell_weather_param(const char* name, float fallback) {
    auto* env = fjell::script_services().environment;
    if (!env || !name) return fallback;
    for (const auto& value : env->state().weather.params) {
        if (value.name == name) return value.values[0];
    }
    return fallback;
}

extern "C" FJELL_SDK_EXPORT void fjell_weather_set_sky_param(const char* name, int count, const float* values) {
    auto* env = fjell::script_services().environment;
    if (!env || !name) return;
    fjell::NamedValue value;
    value.name = name;
    value.count = static_cast<uint32_t>(std::clamp(count, 0, 4));
    for (uint32_t i = 0; i < value.count && values; ++i) value.values[i] = values[i];
    env->set_sky_uniform(value);
}

extern "C" FJELL_SDK_EXPORT void fjell_weather_set_climate(uint32_t climate_handle_id) {
    auto* env = fjell::script_services().environment;
    if (env) env->set_climate(fjell::ClimateAssetHandle{climate_handle_id});
}

extern "C" FJELL_SDK_EXPORT void fjell_weather_hold(int held) {
    auto* env = fjell::script_services().environment;
    if (env) env->hold_weather(held != 0);
}

extern "C" FJELL_SDK_EXPORT int fjell_weather_is_held() {
    auto* env = fjell::script_services().environment;
    return env && env->weather_held() ? 1 : 0;
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_weather_forecast(uint32_t index) {
    auto* env = fjell::script_services().environment;
    auto* resources = fjell::script_services().resources;
    if (!env || !resources) return 0;
    const std::string path = env->forecast_event(index);
    if (path.empty()) return 0;
    auto handle = resources->load_weather_asset(path);
    return handle ? handle->id : 0u;
}
