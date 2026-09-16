#include "environment/environment_subsystem.hpp"

#include "core/frame_context.hpp"
#include "environment/environment_resolver.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <algorithm>

namespace fjell {

void EnvironmentSubsystem::tick(const TickContext& ctx) {
    scene_ = &ctx.scene;

    auto* env = find_world_environment(ctx.scene);
    if (env != nullptr && env->time_of_day_enabled) {
        const bool advancing = playing() || env->advance_in_editor;
        if (advancing && env->day_length > 0.0f) {
            env->time_of_day = wrap_hour(env->time_of_day + ctx.dt / env->day_length * 24.0f);
        }
    }

    state_ = resolve_environment(ctx.scene, moon_led_);
    moon_led_ = moon_leads(state_.sun, state_.moon, moon_led_);

    // The sun light is driven, not authored, while a time of day runs: keep
    // the node's direction at the resolved value so everything that reads
    // the node (inspector, gizmos, a saved scene) sees the same sun.
    if (env != nullptr && env->time_of_day_enabled) {
        for (const auto& obj : ctx.scene.objects()) {
            if (auto* dl = dynamic_cast<DirectionalLightNode*>(obj.get())) {
                dl->direction = -state_.sun.direction;
                break;
            }
        }
    }
}

void EnvironmentSubsystem::set_hour(float hour) {
    if (scene_ == nullptr) return;
    if (auto* env = find_world_environment(*scene_)) {
        env->time_of_day = wrap_hour(hour);
    }
}

void EnvironmentSubsystem::set_day_length(float seconds) {
    if (scene_ == nullptr) return;
    if (auto* env = find_world_environment(*scene_)) {
        env->day_length = std::max(seconds, 0.0f);
    }
}

float EnvironmentSubsystem::day_length() const {
    if (scene_ == nullptr) return 0.0f;
    const auto* env = find_world_environment(*scene_);
    return env != nullptr ? env->day_length : 0.0f;
}

} // namespace fjell
