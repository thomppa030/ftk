#include "environment/environment_subsystem.hpp"

#include "core/frame_context.hpp"
#include "core/log.hpp"
#include "audio/audio_subsystem.hpp"
#include "scripting/script_subsystem.hpp"
#include "core/base_path.hpp"
#include "core/project.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_subsystem.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "environment/climate_asset.hpp"
#include "environment/day_profile_asset.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/water_surface.hpp"
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
        bind_climate(*env, ctx.resources);
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
    // A climate keeps the weather going: when an event runs out, the
    // director rolls the next one for the hour it ends at.
    if (director_.active() && player_.active() == nullptr) {
        (void)director_.advance(state_.hour);
        play_directed(ctx.resources);
    }
    const WeatherSkyOverride sky = player_.sky_override();
    state_ = resolve_environment(ctx.scene, moon_led_, &sky, ctx.dt);
    player_.composite(state_, ctx.dt, rates_);
    for (const auto& value : script_sky_uniforms_) {
        auto it = std::find_if(state_.sky_uniforms.begin(), state_.sky_uniforms.end(),
                               [&](const NamedValue& n) { return n.name == value.name; });
        if (it != state_.sky_uniforms.end()) *it = value;
        else state_.sky_uniforms.push_back(value);
    }
    moon_led_ = moon_leads(state_.sun, state_.moon, moon_led_);
    advance_fog_volumes(ctx);
    update_beds(ctx);
    update_precipitation(ctx);
    update_lightning(ctx);
    dispatch_changes(ctx);

    update_water(ctx);

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
    play_weather(*handle, env.weather_start_phase);
}

void EnvironmentSubsystem::bind_climate(WorldEnvironmentNode& env, ResourceManager* resources) {
    if (env.climate == env.climate_bound) {
        if (resources != nullptr && env.climate_handle.valid()) {
            env.climate_asset = resources->get(env.climate_handle);
        }
        return;
    }
    env.climate_bound = env.climate;
    env.climate_handle = {};
    env.climate_asset = nullptr;
    if (env.climate.empty()) {
        director_.set_climate(nullptr, state_.hour);
        return;
    }
    if (resources == nullptr) return;

    auto handle = resources->load_climate_asset(env.climate);
    if (!handle) {
        FJELL_CORE_ERROR("World environment: {}", handle.error());
        return;
    }
    env.climate_handle = *handle;
    env.climate_asset = resources->get(*handle);
    director_.set_climate(env.climate_asset, state_.hour);
    // The climate decides from here; its first state starts now rather than
    // waiting for the scene's own weather to run out.
    play_directed(resources);
}

void EnvironmentSubsystem::set_climate(ClimateAssetHandle climate) {
    if (resources_ == nullptr) return;
    // The node keeps naming the scene's climate; a script's choice replaces
    // the director's table without rewriting the scene.
    const ClimateAsset* asset = climate.valid() ? resources_->get(climate) : nullptr;
    director_.set_climate(asset, state_.hour);
    if (asset != nullptr) play_directed(resources_);
}

void EnvironmentSubsystem::play_directed(ResourceManager* resources) {
    const ClimateAsset* climate = director_.climate();
    if (climate == nullptr || resources == nullptr) return;
    const ClimateRoll& roll = director_.current();
    if (!roll.valid() || static_cast<size_t>(roll.state_index) >= climate->states.size()) return;
    const std::string& event = climate->states[static_cast<size_t>(roll.state_index)].event;
    auto handle = resources->load_weather_asset(event);
    if (!handle) {
        FJELL_CORE_ERROR("Climate '{}': {}", climate->name, handle.error());
        return;
    }
    play_weather(*handle);
}

std::string EnvironmentSubsystem::forecast_event(uint32_t index) const {
    const ClimateAsset* climate = director_.climate();
    if (climate == nullptr) return {};
    const auto& forecast = director_.forecast();
    if (index >= forecast.size()) return {};
    const int state = forecast[index].state_index;
    if (state < 0 || static_cast<size_t>(state) >= climate->states.size()) return {};
    return climate->states[static_cast<size_t>(state)].event;
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

void EnvironmentSubsystem::play_weather(WeatherAssetHandle event, WeatherPhase from) {
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
    // A fixed base replaces the play count rather than adding to it, so the
    // seed does not depend on how many events happened to play before this
    // one — which is the whole point of fixing it.
    const uint32_t roll = seed_base_ != 0 ? seed_base_ : play_count_ * 7919u;
    player_.play(asset, event.id, roll + event.id, profile_for(*asset), from);

    // Starting partway through is someone asking to see that moment, not
    // weather arriving: the editor jumping to a phase, or a scene opening
    // mid-storm. The sea belongs at that moment too, rather than spending the
    // next half minute catching up to a front that is already overhead.
    // An event started from the beginning is a front approaching normally,
    // and that one the sea answers in its own time.
    if (from != WeatherPhase::Approach) {
        sea_wind_.reset();
    }
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

namespace {
// How far above the camera the falling volume sits. Far enough that drops
// have fallen into their stride by the time they pass the eye, close enough
// that they are still alive when they land.
constexpr float PRECIPITATION_HEIGHT = 15.0f;
} // namespace

void EnvironmentSubsystem::stop_precipitation(Scene& scene) {
    for (Precipitation* fall : {&active_fall_, &outgoing_fall_}) {
        if (fall->node_id != 0) scene.queue_destroy(fall->node_id);
        *fall = Precipitation{};
    }
}

void EnvironmentSubsystem::advance_fog_volumes(const TickContext& ctx) {
    const auto& weather = state_.weather;
    // Sustained wind, not the gust: a gust breathes about once a second and
    // fog has more mass than that.
    const glm::vec3 wind_dir = glm::length(weather.wind_direction) > 1e-4f
                                   ? glm::normalize(weather.wind_direction)
                                   : glm::vec3(0.0f, 0.0f, 1.0f);

    for (const auto& obj : ctx.scene.objects()) {
        auto* volume = dynamic_cast<FogVolumeNode*>(obj.get());
        if (volume == nullptr || volume->noise_contrast <= 0.0f) continue;

        const glm::vec3 authored = glm::length(volume->noise_direction) > 1e-4f
                                       ? glm::normalize(volume->noise_direction)
                                       : glm::vec3(0.0f, 0.0f, 1.0f);

        // The wind takes over the heading and the pace together, so a volume
        // fully given to it runs downwind at the wind's own speed and stalls
        // when the air is still. Fog does not travel as fast as the air that
        // carries it, hence the scale.
        const float influence = glm::clamp(volume->wind_influence, 0.0f, 1.0f);
        const glm::vec3 direction = glm::normalize(glm::mix(authored, wind_dir, influence));
        const float wind_pace = weather.wind_speed * volume->wind_speed_scale;
        const float speed = glm::mix(volume->noise_speed, wind_pace, influence);

        volume->noise_drift_ += direction * speed * ctx.dt;
    }
}

void EnvironmentSubsystem::update_precipitation(const TickContext& ctx) {
    if (resources_ == nullptr) return;
    // Falling weather belongs to play, like the sounds: an editor scrub
    // should not fill the viewport with rain nobody asked for.
    if (!playing()) {
        stop_precipitation(ctx.scene);
        return;
    }

    // Where the rain falls: around whoever is looking.
    glm::vec3 centre = state_.weather.wind_direction * 0.0f;
    for (const auto& obj : ctx.scene.objects()) {
        if (const auto* cam = dynamic_cast<const CameraNode*>(obj.get())) {
            centre = glm::vec3(cam->world_matrix_[3]);
            break;
        }
    }

    auto follow = [&](Precipitation& fall, const WeatherPlayer::Playing* playing_event) {
        // Which effect this event drops, if any.
        std::string wanted;
        uint32_t event_id = 0;
        float rate = 0.0f;
        if (playing_event != nullptr && playing_event->asset != nullptr) {
            const WeatherAsset& asset = *playing_event->asset;
            event_id = playing_event->event_id;
            if (asset.precipitation_type == PrecipitationType::Rain) wanted = asset.vfx.rain;
            else if (asset.precipitation_type == PrecipitationType::Snow) wanted = asset.vfx.snow;
            rate = state_.weather.precipitation * playing_event->weight();
        }

        if (wanted.empty() || rate <= 0.001f) {
            if (fall.node_id != 0) {
                ctx.scene.queue_destroy(fall.node_id);
                fall = Precipitation{};
            }
            return;
        }

        if (fall.node_id == 0 || fall.path != wanted || fall.event_id != event_id) {
            if (fall.node_id != 0) ctx.scene.queue_destroy(fall.node_id);
            fall = Precipitation{};
            auto handle = resources_->load_vfx_asset(base_path::resolve_asset(wanted, ctx.project));
            if (!handle) {
                FJELL_CORE_ERROR("Weather precipitation: {}", handle.error());
                return;
            }
            auto node = std::make_unique<Node3D>();
            node->name = "Weather Precipitation";
            auto& emitter = node->components.add<EmitterComponent>();
            emitter.vfx_path = wanted;
            emitter.effect = std::shared_ptr<VfxEffect>(resources_->get(*handle), [](VfxEffect*) {});
            emitter.looping = true;
            fall.node_id = ctx.scene.add_node(std::move(node));
            fall.path = wanted;
            fall.event_id = event_id;
        }

        // Park it above the camera. The effect's own spawn volume decides
        // how wide and how high the drops start, so authoring stays in the
        // VFX editor; this only keeps that volume overhead.
        if (auto* node = ctx.scene.find_spatial(fall.node_id)) {
            node->transform.position = centre + glm::vec3(0.0f, PRECIPITATION_HEIGHT, 0.0f);
        }
    };

    follow(active_fall_, player_.active());
    follow(outgoing_fall_, player_.outgoing());
}

void EnvironmentSubsystem::strike_lightning(const glm::vec3& relative_position, float intensity) {
    player_.strike(relative_position, intensity);
}

void EnvironmentSubsystem::stop_lightning(Scene& scene) {
    for (const auto& light : strike_lights_) {
        if (light.node_id != 0) scene.queue_destroy(light.node_id);
    }
    strike_lights_.clear();
    pending_thunder_.clear();
}

void EnvironmentSubsystem::update_water(const TickContext& ctx) {
    // The sustained wind, deliberately without the gust that a sail or a
    // leaf gets. A gust breathes on about a one-second cycle, and a sea that
    // answered it would swing its whole height with it — measured at a gale,
    // between 0.93x and 2.27x every second, the entire ocean heaving. Real
    // water does not work that way: wind takes minutes to hours to raise a
    // sea, and what a gust does meanwhile is ruffle the surface, which is the
    // material's business and not the wave field's.
    const glm::vec3 wind = state_.weather.wind_direction * state_.weather.wind_speed;
    const glm::vec2 blowing{wind.x, wind.z};

    // Water has mass: the field follows the wind rather than tracking it, so
    // a front raises a sea over its passage instead of at its arrival. The
    // curve and its rates live beside the evaluator, where they are tested.
    if (!sea_wind_.has_value()) {
        // First tick: start at what is already blowing, so a scene opens with
        // the sea its weather implies instead of building one up from calm.
        sea_wind_ = blowing;
    }
    sea_wind_ = water::advance_sea_wind(*sea_wind_, blowing, ctx.dt);

    const glm::vec2 wind_xz = *sea_wind_;

    // Unlike rain and lightning this runs in the editor as well as in play.
    // A sea's answer to the wind is something an artist tunes by watching it,
    // and the weather editor scrubs a front past a scene precisely so they
    // can; stopping at the play boundary would leave that dial blind.
    for (const auto& obj : ctx.scene.objects()) {
        auto* water = obj->components.get<WaterComponent>();
        if (water == nullptr) continue;
        water->effective_water = water->derivation.derive(water->water, wind_xz, ctx.workers);
    }
}

void EnvironmentSubsystem::update_lightning(const TickContext& ctx) {
    if (!playing()) {
        stop_lightning(ctx.scene);
        return;
    }

    // Where the listener is, so a strike lands somewhere around them.
    glm::vec3 listener{0.0f};
    for (const auto& obj : ctx.scene.objects()) {
        if (const auto* cam = dynamic_cast<const CameraNode*>(obj.get())) {
            listener = glm::vec3(cam->world_matrix_[3]);
            break;
        }
    }

    // A new strike: its own light, its bolt, and thunder on its way.
    if (const auto* strike = player_.strike_this_tick(); strike != nullptr && strike->asset != nullptr) {
        const auto& l = strike->asset->lightning;
        const glm::vec3 world = listener + strike->position;
        const float distance = glm::length(strike->position);

        auto node = std::make_unique<PointLightNode>();
        node->name = "Lightning";
        node->transform.position = world + glm::vec3(0.0f, l.radius * 0.5f, 0.0f);
        node->color = l.color;
        node->intensity = 0.0f;   // the flash curve drives it from here
        node->radius = l.radius;
        StrikeLight light;
        light.node_id = ctx.scene.add_node(std::move(node));
        light.remaining = l.duration;
        light.duration = l.duration;
        light.intensity = l.intensity * strike->intensity;
        strike_lights_.push_back(light);

        if (!l.vfx.empty() && resources_ != nullptr && vfx_ != nullptr) {
            if (auto handle = resources_->load_vfx_asset(base_path::resolve_asset(l.vfx, ctx.project))) {
                vfx_->spawn(*handle, world, ctx.scene, *resources_);
            }
        }
        if (!strike->asset->audio.thunder.empty()) {
            // Sound crosses the distance at its own pace; that gap is what
            // tells you how far the storm is.
            pending_thunder_.push_back(PendingThunder{
                .delay = distance / 343.0f,
                .volume = glm::clamp(1.0f - distance / std::max(l.max_distance, 1.0f), 0.15f, 1.0f),
                .position = world,
                .clip = strike->asset->audio.thunder});
        }
    }

    // The lights follow the same flash the sky does, then go.
    const float flash = state_.weather.lightning;
    for (size_t i = strike_lights_.size(); i-- > 0;) {
        StrikeLight& light = strike_lights_[i];
        light.remaining -= ctx.dt;
        if (light.remaining <= 0.0f) {
            ctx.scene.queue_destroy(light.node_id);
            strike_lights_.erase(strike_lights_.begin() + static_cast<long>(i));
            continue;
        }
        if (auto* node = dynamic_cast<PointLightNode*>(ctx.scene.find(light.node_id))) {
            node->intensity = light.intensity * flash;
        }
    }

    // Thunder arrives when it arrives.
    for (size_t i = pending_thunder_.size(); i-- > 0;) {
        PendingThunder& thunder = pending_thunder_[i];
        thunder.delay -= ctx.dt;
        if (thunder.delay > 0.0f) continue;
        if (audio_ != nullptr) {
            audio_->engine().play_oneshot(thunder.clip, thunder.position, thunder.volume,
                                          1.0f, ctx.project);
        }
        pending_thunder_.erase(pending_thunder_.begin() + static_cast<long>(i));
    }
}

void EnvironmentSubsystem::dispatch_changes(const TickContext& ctx) {
    // Only while playing: an editor scrub is not something a script should
    // be told about, and nothing is running to hear it.
    if (!playing() || scripts_ == nullptr) {
        phases_seen_ = false;
        return;
    }
    const uint32_t event = state_.weather.event_id;
    const WeatherPhase weather_phase = state_.weather.phase;
    const TimePhase time_phase = state_.phase;
    if (!phases_seen_) {
        // The first playing frame is the baseline, not a change.
        phases_seen_ = true;
    } else {
        if (event != last_event_ || weather_phase != last_weather_phase_) {
            scripts_->manager().dispatch_weather_changed(ctx.scene, event, static_cast<int>(weather_phase));
        }
        if (time_phase != last_time_phase_) {
            scripts_->manager().dispatch_time_phase_changed(ctx.scene, static_cast<int>(time_phase));
        }
    }
    last_event_ = event;
    last_weather_phase_ = weather_phase;
    last_time_phase_ = time_phase;
}

void EnvironmentSubsystem::on_play_stop(const TickContext& ctx) {
    stop_beds(active_beds_);
    stop_beds(outgoing_beds_);
    stop_precipitation(ctx.scene);
    stop_lightning(ctx.scene);
    phases_seen_ = false;
    // Leaving play puts the editor's own weather back; the sea starts from
    // that rather than easing down from whatever the session ended in.
    sea_wind_.reset();
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
