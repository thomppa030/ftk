#include "environment/environment_subsystem.hpp"

#include "core/frame_context.hpp"
#include "core/log.hpp"
#include "audio/audio_subsystem.hpp"
#include "core/project.hpp"
#include "environment/day_profile_asset.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "environment/environment_resolver.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <algorithm>

namespace fjell {

void EnvironmentSubsystem::tick(const TickContext& ctx) {
    scene_ = &ctx.scene;
    resources_ = ctx.resources;
    if (ctx.project != nullptr) {
        const auto& cfg = ctx.project->config().environment;
        rates_ = WeatherRates{cfg.wetting_seconds, cfg.drying_seconds, cfg.snowing_seconds, cfg.melting_seconds};
    }

    auto* env = find_world_environment(ctx.scene);
    if (env != nullptr) {
        bind_day_profile(*env, ctx.resources);
        bind_weather(*env, ctx.resources);
    }
    if (env != nullptr && env->time_of_day_enabled) {
        const bool advancing = playing() || env->advance_in_editor;
        if (advancing && env->day_length > 0.0f) {
            env->time_of_day = wrap_hour(env->time_of_day + ctx.dt / env->day_length * 24.0f);
        }
    }

    // Weather runs on game time like the clock: paused with it, scaled with it.
    player_.tick(ctx.dt);
    const WeatherSkyOverride sky = player_.sky_override();
    state_ = resolve_environment(ctx.scene, moon_led_, &sky);
    player_.composite(state_, ctx.dt, rates_);
    for (const auto& value : script_sky_uniforms_) {
        auto it = std::find_if(state_.sky_uniforms.begin(), state_.sky_uniforms.end(),
                               [&](const NamedValue& n) { return n.name == value.name; });
        if (it != state_.sky_uniforms.end()) *it = value;
        else state_.sky_uniforms.push_back(value);
    }
    moon_led_ = moon_leads(state_.sun, state_.moon, moon_led_);
    update_beds(ctx);

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

void EnvironmentSubsystem::bind_day_profile(WorldEnvironmentNode& env, ResourceManager* resources) {
    if (env.day_profile == env.day_profile_bound) {
        // Same path: keep following the pool's object, which a reload of the
        // asset replaces in place.
        if (resources != nullptr && env.day_profile_handle.valid()) {
            env.day_profile_asset = resources->get(env.day_profile_handle);
        }
        return;
    }
    env.day_profile_bound = env.day_profile;
    env.day_profile_handle = {};
    env.day_profile_asset = nullptr;
    if (env.day_profile.empty() || resources == nullptr) return;

    auto handle = resources->load_day_profile(env.day_profile);
    if (!handle) {
        FJELL_CORE_ERROR("World environment: {}", handle.error());
        return;
    }
    env.day_profile_handle = *handle;
    env.day_profile_asset = resources->get(*handle);
}

void EnvironmentSubsystem::bind_weather(WorldEnvironmentNode& env, ResourceManager* resources) {
    if (env.weather == env.weather_bound) {
        if (resources != nullptr && env.weather_handle.valid()) {
            env.weather_asset = resources->get(env.weather_handle);
        }
        return;
    }
    env.weather_bound = env.weather;
    env.weather_handle = {};
    env.weather_asset = nullptr;
    if (env.weather.empty()) {
        // The scene's weather was cleared: whatever plays tails out.
        player_.stop();
        return;
    }
    if (resources == nullptr) return;

    auto handle = resources->load_weather_asset(env.weather);
    if (!handle) {
        FJELL_CORE_ERROR("World environment: {}", handle.error());
        return;
    }
    env.weather_handle = *handle;
    env.weather_asset = resources->get(*handle);
    play_weather(*handle);
}

const DayProfileAsset* EnvironmentSubsystem::profile_for(const WeatherAsset& event) const {
    if (event.day_profile.empty() || resources_ == nullptr) return nullptr;
    auto handle = resources_->load_day_profile(event.day_profile);
    if (!handle) {
        FJELL_CORE_ERROR("Weather '{}': {}", event.name, handle.error());
        return nullptr;
    }
    return resources_->get(*handle);
}

void EnvironmentSubsystem::play_weather(WeatherAssetHandle event) {
    if (!event.valid() || resources_ == nullptr) {
        player_.stop();
        return;
    }
    const WeatherAsset* asset = resources_->get(event);
    if (asset == nullptr) {
        player_.stop();
        return;
    }
    ++play_count_;
    player_.play(asset, event.id, play_count_ * 7919u + event.id, profile_for(*asset));
}

void EnvironmentSubsystem::set_sky_uniform(const NamedValue& value) {
    auto it = std::find_if(script_sky_uniforms_.begin(), script_sky_uniforms_.end(),
                           [&](const NamedValue& n) { return n.name == value.name; });
    if (value.count == 0) {
        if (it != script_sky_uniforms_.end()) script_sky_uniforms_.erase(it);
        return;
    }
    if (it != script_sky_uniforms_.end()) *it = value;
    else script_sky_uniforms_.push_back(value);
}

void EnvironmentSubsystem::on_play_stop(const TickContext& /*ctx*/) {
    stop_beds(active_beds_);
    stop_beds(outgoing_beds_);
}

void EnvironmentSubsystem::stop_beds(Beds& beds) {
    if (audio_ != nullptr) {
        if (beds.bed != 0) audio_->engine().stop_bed(beds.bed);
        if (beds.wind != 0) audio_->engine().stop_bed(beds.wind);
    }
    beds = Beds{};
}

void EnvironmentSubsystem::follow_event(Beds& beds, const WeatherPlayer::Playing* playing,
                                        const TickContext& ctx) {
    if (playing == nullptr || playing->asset == nullptr) {
        stop_beds(beds);
        return;
    }
    if (beds.event_id != playing->event_id || beds.asset != playing->asset) {
        stop_beds(beds);
        beds.event_id = playing->event_id;
        beds.asset = playing->asset;
        auto& engine = audio_->engine();
        if (!playing->asset->audio.bed.empty()) beds.bed = engine.play_bed(playing->asset->audio.bed, 0.0f, ctx.project);
        if (!playing->asset->audio.wind.empty()) beds.wind = engine.play_bed(playing->asset->audio.wind, 0.0f, ctx.project);
    }
    // The bed follows the event's presence and, when it precipitates, how
    // hard; the wind bed follows the wind itself.
    const auto& w = state_.weather;
    const float weight = playing->weight();
    const bool precipitates = playing->asset->precipitation_type != PrecipitationType::None;
    const float bed_volume = weight * (precipitates ? w.precipitation : 1.0f);
    const float wind_volume = glm::clamp(w.wind_speed * w.wind_gust / 12.0f, 0.0f, 1.0f) * weight;
    if (beds.bed != 0) audio_->engine().set_bed_volume(beds.bed, bed_volume);
    if (beds.wind != 0) audio_->engine().set_bed_volume(beds.wind, wind_volume);
}

void EnvironmentSubsystem::update_beds(const TickContext& ctx) {
    if (audio_ == nullptr) return;
    // Weather sounds belong to play, like every other sound in the scene.
    if (!playing()) {
        stop_beds(active_beds_);
        stop_beds(outgoing_beds_);
        return;
    }
    follow_event(active_beds_, player_.active(), ctx);
    follow_event(outgoing_beds_, player_.outgoing(), ctx);
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
