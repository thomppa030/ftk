#include "environment/environment_overrides.hpp"

#include "core/log.hpp"
#include "environment/environment_resolver.hpp"
#include "environment/environment_subsystem.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

namespace fjell {

void apply_environment_overrides(const EnvironmentOverrides& overrides,
                                 Scene& scene,
                                 EnvironmentSubsystem& environment,
                                 ResourceManager& resources) {
    WorldEnvironmentNode* env = find_world_environment(scene);
    if (env == nullptr) {
        FJELL_CORE_WARN("Environment overrides: the scene has no World Environment node, "
                        "so there is nothing to override");
        return;
    }

    // Before anything is played, so the first event rolls from the fixed
    // seed rather than the default one.
    if (overrides.seed) {
        environment.set_seed_base(*overrides.seed);
    }

    // These go on the node rather than through the subsystem's setters
    // because the subsystem rebinds from the node every tick: a value set
    // beside the node is the one the next tick overwrites.
    if (overrides.hour) {
        env->time_of_day_enabled = true;
        env->time_of_day = *overrides.hour;
    }
    if (overrides.day_length) {
        env->day_length = *overrides.day_length;
    }
    if (overrides.weather_phase) {
        env->weather_start_phase = *overrides.weather_phase;
    }
    if (overrides.weather) {
        // Checked here so a bad path is reported now, with the flag that
        // named it, rather than as a load failure during the first tick.
        if (auto handle = resources.load_weather_asset(*overrides.weather); !handle) {
            FJELL_CORE_ERROR("Weather override: {}", handle.error());
        } else {
            env->weather = *overrides.weather;
        }
    }
    // Clearing what the node was last bound to is what makes the next tick
    // start the event again — in the phase just set, and with the seed.
    if (overrides.weather || overrides.weather_phase) {
        env->weather_bound.clear();
    }

    if (overrides.hold_weather) {
        environment.hold_weather(*overrides.hold_weather);
    }

    // Last, because starting an event above does not disturb these but
    // reading them in the wrong order would invite it to. They survive the
    // rebind on the next tick: the player integrates from whatever it holds,
    // and nothing resets it when an event starts.
    if (overrides.wetness) {
        environment.set_wetness(*overrides.wetness);
    }
    if (overrides.snow) {
        environment.set_snow(*overrides.snow);
    }
}

} // namespace fjell
