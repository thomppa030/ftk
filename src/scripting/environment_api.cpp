// environment_api.cpp — C ABI bridge from game .so to the environment subsystem
#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "environment/environment_subsystem.hpp"

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
