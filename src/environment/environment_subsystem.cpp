#include "environment/environment_subsystem.hpp"

#include "core/clock.hpp"
#include "core/frame_context.hpp"
#include "core/log.hpp"
#include "audio/audio_subsystem.hpp"
#include "scripting/script_subsystem.hpp"
#include "core/base_path.hpp"
#include "core/project.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_module.hpp"
#include "renderer/systems/vfx_subsystem.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "environment/climate_asset.hpp"
#include "environment/day_profile_asset.hpp"
#include "renderer/bindless.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/resources/water_surface.hpp"
#include "environment/environment_resolver.hpp"
#include "scene/light_node.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <span>

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
    bind_node_materials(ctx.scene, ctx.resources, ctx.project);
    if (env != nullptr && env->time_of_day_enabled) {
        const bool advancing = playing() || env->advance_in_editor;
        if (advancing && env->day_length > 0.0f) {
            env->time_of_day = wrap_hour(env->time_of_day + ctx.dt / env->day_length * 24.0f);
        }
    }

    // Weather runs on game time like the clock: paused with it, scaled with it.
    SceneWeather& weather = scene_weather_;
    weather.player.tick(ctx.dt);
    // A climate keeps the weather going: when an event runs out, the
    // director rolls the next one for the hour it ends at.
    if (weather.director.active() && weather.player.active() == nullptr) {
        (void)weather.director.advance(state_.hour);
        play_directed(ctx.resources);
    }
    const WeatherSkyOverride sky = weather.player.sky_override();
    state_ = resolve_environment(ctx.scene, weather.moon_led, &sky, ctx.dt);
    weather.player.composite(state_, ctx.dt, rates_);
    for (const auto& value : weather.script_sky_uniforms) {
        auto it = std::find_if(state_.sky_uniforms.begin(), state_.sky_uniforms.end(),
                               [&](const NamedValue& n) { return n.name == value.name; });
        if (it != state_.sky_uniforms.end()) *it = value;
        else state_.sky_uniforms.push_back(value);
    }
    weather.moon_led = moon_leads(state_.sun, state_.moon, weather.moon_led);
    advance_fog_volumes(ctx);
    update_beds(ctx);
    update_precipitation(ctx);
    update_lightning(ctx);
    dispatch_changes(ctx);

    update_water_media(ctx);
    // While the game runs, the simulation advances the water in its own
    // steps (see fixed_tick). Outside play the frame does it: a sea's answer
    // to the wind is something an artist tunes by watching it, and the
    // weather editor scrubs a front past a scene precisely so they can;
    // stopping at the play boundary would leave that dial blind.
    if (!playing()) {
        advance_water(ctx.scene, state_.motion_sources, ctx.dt, ctx.clock.game_elapsed(),
                      ctx.workers);
    }
    update_breathing(ctx);

    // The sun light is driven, not authored, while a time of day runs: keep
    // the node's direction at the resolved value so everything that reads
    // the node (inspector, gizmos, a saved scene) sees the same sun.
    if (env != nullptr && env->time_of_day_enabled) {
        if (auto* dl = ctx.scene.first_node<DirectionalLightNode>()) {
            dl->direction = -state_.sun.direction;
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

namespace {

// Every scalar and vector uniform of the shader the node draws with, at the
// material's own value: read back from the material's row so a .fjmat edited
// in the inspector or reloaded from disk is what the resolver seeds from. A
// node with no material reads the default shader's declared values.
void read_material_values(const Material* material, std::string_view default_shader,
                          ShaderType type, std::vector<NamedValue>& out,
                          ResourceManager* resources) {
    out.clear();
    if (resources == nullptr) return;
    PipelineCache* cache = resources->pipeline_cache();
    if (cache == nullptr) return;

    const uint32_t pid = material != nullptr
        ? material->pipeline_id()
        : cache->register_shader(std::string(default_shader));
    if (pid >= cache->shader_count()) return;
    const auto& meta = cache->metadata(pid);
    if (meta.shader_type != type) return;

    const BindlessTextures& bindless = resources->bindless();
    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;
        NamedValue value;
        value.name = u.name;
        value.count = uniform_float_count(u.type);
        value.srgb = u.hint == UniformHint::Color;
        if (material != nullptr) {
            for (uint32_t i = 0; i < value.count; ++i) {
                value.values[i] = bindless.read_param(material->custom_param_offset() + u.slot + i);
            }
            decode_uniform_from_gpu(u, std::span{value.values}.first(value.count));
        } else {
            for (uint32_t i = 0; i < value.count && i < u.default_value.size(); ++i) {
                value.values[i] = u.default_value[i];
            }
        }
        out.push_back(std::move(value));
    }
}

// The haze, a fog volume and a post process volume carry their material the
// same way; this keeps one node's material following its path and reads its
// values back. `type` is the shader_type the node draws with, `what` names
// the node in the log, and `light_unused` says the material's light() never
// runs on this node: only the haze's lights the fog, so a fog volume's is
// reported rather than silently ignored.
template <typename MaterialNode>
void bind_node_material(MaterialNode& node, std::string_view default_shader, ShaderType type,
                        const char* what, bool light_unused, ResourceManager* resources,
                        const Project* project) {
    if (node.material_path != node.material_bound_) {
        node.material_bound_ = node.material_path;
        node.material_handle = {};
        node.material = nullptr;
        if (!node.material_path.empty() && resources != nullptr) {
            auto handle = resources->load_material_asset(
                base_path::resolve_asset(node.material_path, project), project);
            if (!handle) {
                FJELL_CORE_ERROR("{}: {}", what, handle.error());
            } else {
                Material* material = resources->get(*handle);
                if (material != nullptr && material->shader_type() != type) {
                    // Bound to nothing rather than to the wrong kind of
                    // shader, so the node draws the engine's default and says
                    // why once instead of failing to build a pipeline every
                    // frame.
                    FJELL_CORE_ERROR("{}: '{}' is shader_type '{}', this node needs shader_type "
                                     "{}; drawing the default",
                                     what, node.material_path,
                                     shader_type_name(material->shader_type()),
                                     shader_type_name(type));
                } else {
                    node.material_handle = *handle;
                    node.material = material;
                    if (material != nullptr && light_unused && resources->pipeline_cache() != nullptr) {
                        const auto& meta = resources->pipeline_cache()->metadata(material->pipeline_id());
                        if (!meta.light_body.empty()) {
                            FJELL_CORE_ERROR("{}: '{}' defines light(), which only the Volumetric "
                                             "Fog node's material runs; the volume's medium draws, "
                                             "its light() is ignored",
                                             what, node.material_path);
                        }
                    }
                }
            }
        }
    } else if (resources != nullptr && node.material_handle.valid()) {
        // Same path: keep following the pool's object, which a reload of the
        // asset replaces in place.
        node.material = resources->get(node.material_handle);
    }
    read_material_values(node.material, default_shader, type, node.material_values_, resources);
}

} // namespace

void EnvironmentSubsystem::bind_node_materials(Scene& scene, ResourceManager* resources,
                                               const Project* project) {
    scene.each_node<VolumetricFogNode>([&](VolumetricFogNode& fog) {
        bind_node_material(fog, DEFAULT_FOG_SHADER, ShaderType::Fog, "Volumetric fog", false,
                           resources, project);
    });
    scene.each_node<FogVolumeNode>([&](FogVolumeNode& volume) {
        bind_node_material(volume, DEFAULT_FOG_VOLUME_SHADER, ShaderType::Fog, "Fog volume", true,
                           resources, project);
    });
    scene.each_node<PostProcessVolumeNode>([&](PostProcessVolumeNode& post) {
        bind_node_material(post, DEFAULT_POST_SHADER, ShaderType::Post, "Post process volume",
                           false, resources, project);
    });
}

void EnvironmentSubsystem::bind_weather(const WorldEnvironmentNode& env, ResourceManager* resources) {
    if (env.weather == scene_weather_.bound_weather) return;
    scene_weather_.bound_weather = env.weather;
    if (env.weather.empty()) {
        // The scene's weather was cleared: whatever plays tails out.
        scene_weather_.player.stop();
        return;
    }
    if (resources == nullptr) return;

    auto handle = resources->load_weather_asset(env.weather);
    if (!handle) {
        FJELL_CORE_ERROR("World environment: {}", handle.error());
        return;
    }
    play_weather(*handle, env.weather_start_phase);
}

void EnvironmentSubsystem::bind_climate(const WorldEnvironmentNode& env, ResourceManager* resources) {
    if (env.climate == scene_weather_.bound_climate) return;
    scene_weather_.bound_climate = env.climate;
    if (env.climate.empty()) {
        scene_weather_.director.set_climate(nullptr, state_.hour);
        return;
    }
    if (resources == nullptr) return;

    auto handle = resources->load_climate_asset(env.climate);
    if (!handle) {
        FJELL_CORE_ERROR("World environment: {}", handle.error());
        return;
    }
    scene_weather_.director.set_climate(resources->get(*handle), state_.hour);
    // The climate decides from here; its first state starts now rather than
    // waiting for the scene's own weather to run out.
    play_directed(resources);
}

void EnvironmentSubsystem::set_climate(ClimateAssetHandle climate) {
    if (resources_ == nullptr) return;
    // The node keeps naming the scene's climate; a script's choice replaces
    // the director's table without rewriting the scene.
    const ClimateAsset* asset = climate.valid() ? resources_->get(climate) : nullptr;
    scene_weather_.director.set_climate(asset, state_.hour);
    if (asset != nullptr) play_directed(resources_);
}

void EnvironmentSubsystem::play_directed(ResourceManager* resources) {
    const ClimateAsset* climate = scene_weather_.director.climate();
    if (climate == nullptr || resources == nullptr) return;
    const ClimateRoll& roll = scene_weather_.director.current();
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
    const ClimateAsset* climate = scene_weather_.director.climate();
    if (climate == nullptr) return {};
    const auto& forecast = scene_weather_.director.forecast();
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
        scene_weather_.player.stop();
        return;
    }
    const WeatherAsset* asset = resources_->get(event);
    if (asset == nullptr) {
        scene_weather_.player.stop();
        return;
    }
    ++scene_weather_.play_count;
    // A fixed base replaces the play count rather than adding to it, so the
    // seed does not depend on how many events happened to play before this
    // one — which is the whole point of fixing it.
    const uint32_t roll = seed_base_ != 0 ? seed_base_ : scene_weather_.play_count * 7919u;
    scene_weather_.player.play(asset, event.id, roll + event.id, profile_for(*asset), from);

    // Starting partway through is someone asking to see that moment, not
    // weather arriving: the editor jumping to a phase, or a scene opening
    // mid-storm. The sea belongs at that moment too, rather than spending the
    // next half minute catching up to a front that is already overhead.
    // An event started from the beginning is a front approaching normally,
    // and that one the sea answers in its own time.
    if (from != WeatherPhase::Approach) {
        scene_weather_.sea_wind.reset();
    }
}

void EnvironmentSubsystem::set_sky_uniform(const NamedValue& value) {
    std::vector<NamedValue>& uniforms = scene_weather_.script_sky_uniforms;
    auto it = std::find_if(uniforms.begin(), uniforms.end(),
                           [&](const NamedValue& n) { return n.name == value.name; });
    if (value.count == 0) {
        if (it != uniforms.end()) uniforms.erase(it);
        return;
    }
    if (it != uniforms.end()) *it = value;
    else uniforms.push_back(value);
}

namespace {
// How far above the camera the falling volume sits. Far enough that drops
// have fallen into their stride by the time they pass the eye, close enough
// that they are still alive when they land.
constexpr float PRECIPITATION_HEIGHT = 15.0f;
} // namespace

void EnvironmentSubsystem::stop_precipitation(Scene* scene) {
    for (Precipitation* fall : {&active_fall_, &outgoing_fall_}) {
        if (scene != nullptr && fall->node_id != 0) scene->queue_destroy(fall->node_id);
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

    ctx.scene.each_node<FogVolumeNode>([&](FogVolumeNode& node) {
        FogVolumeNode* volume = &node;
        const glm::vec3 authored = glm::length(volume->drift_direction) > 1e-4f
                                       ? glm::normalize(volume->drift_direction)
                                       : glm::vec3(0.0f, 0.0f, 1.0f);

        // The wind takes over the heading and the pace together, so a volume
        // fully given to it runs downwind at the wind's own speed and stalls
        // when the air is still. Fog does not travel as fast as the air that
        // carries it, hence the scale.
        const float influence = glm::clamp(volume->wind_influence, 0.0f, 1.0f);
        const glm::vec3 direction = glm::normalize(glm::mix(authored, wind_dir, influence));
        const float wind_pace = weather.wind_speed * volume->wind_speed_scale;
        const float speed = glm::mix(volume->drift_speed, wind_pace, influence);

        volume->drift_ += direction * speed * ctx.dt;
    });
}

void EnvironmentSubsystem::update_precipitation(const TickContext& ctx) {
    if (resources_ == nullptr) return;
    // Falling weather belongs to play, like the sounds: an editor scrub
    // should not fill the viewport with rain nobody asked for.
    if (!playing()) {
        stop_precipitation(&ctx.scene);
        return;
    }

    // Where the rain falls: around whoever is looking.
    glm::vec3 centre = state_.weather.wind_direction * 0.0f;
    if (const auto* cam = ctx.scene.first_node<CameraNode>()) {
        centre = glm::vec3(cam->world_matrix_[3]);
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
            // Its own copy of the effect: the emitter's playback state lives
            // on it, and the resource pool's copy is shared with whoever
            // else loads the asset.
            std::shared_ptr<VfxEffect> effect;
            try {
                effect = std::make_shared<VfxEffect>(
                    load_vfx_effect(base_path::resolve_asset(wanted, ctx.project)));
                resolve_vfx_textures(*effect, *resources_, ctx.project);
            } catch (const std::exception& e) {
                FJELL_CORE_ERROR("Weather precipitation: {}", e.what());
                return;
            }
            auto node = std::make_unique<Node3D>();
            node->name = "Weather Precipitation";
            auto& emitter = node->components.add<EmitterComponent>();
            emitter.vfx_path = wanted;
            emitter.effect = std::move(effect);
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

    follow(active_fall_, scene_weather_.player.active());
    follow(outgoing_fall_, scene_weather_.player.outgoing());
}

void EnvironmentSubsystem::strike_lightning(const glm::vec3& relative_position, float intensity) {
    scene_weather_.player.strike(relative_position, intensity);
}

void EnvironmentSubsystem::stop_lightning(Scene* scene) {
    for (const auto& light : strike_lights_) {
        if (scene != nullptr && light.node_id != 0) scene->queue_destroy(light.node_id);
    }
    strike_lights_.clear();
    pending_thunder_.clear();
}

void EnvironmentSubsystem::update_water_media(const TickContext& ctx) {
    state_.water_media.clear();
    ctx.scene.each<WaterComponent>([&](const Node& node, WaterComponent& water_component) {
        auto* water = &water_component;

        // What the body of the water is made of, for the fog pass. The
        // material is followed through its handle, which a reload of the
        // asset replaces in place, and its values read now so a hot edit
        // shows this frame.
        if (water->medium_material_handle.valid() && resources_ != nullptr) {
            water->medium_material = resources_->get(water->medium_material_handle);
        }
        if (water->medium_material != nullptr) {
            read_material_values(water->medium_material, DEFAULT_WATER_MEDIUM_SHADER,
                                 ShaderType::Fog, water->medium_values_, resources_);
            WaterMedium medium;
            medium.node_id = node.id;
            if (const auto* spatial = node.as_spatial()) {
                medium.origin = glm::vec3(spatial->world_matrix_[3]);
            }
            medium.pipeline_id = water->medium_material->pipeline_id();
            medium.material_id = water->medium_material->material_id();
            medium.uniforms = water->medium_values_;
            state_.water_media.push_back(std::move(medium));
        }
    });
}

namespace {

/// The modules of an emitter that the breath drives, found once per tick.
struct BreathDials {
    SpawnRateModule* rate{nullptr};
    InitialLifetimeModule* lifetime{nullptr};
    InitialPositionModule* position{nullptr};
    /// How fast a bubble rises once the emitter's buoyancy and drag balance:
    /// the force over the drag. What a bubble's life is measured against.
    float rise_speed{0.5f};
};

[[nodiscard]] BreathDials breath_dials(VfxEmitter& emitter) {
    BreathDials dials;
    for (auto& module : emitter.spawn_modules) {
        if (module->type() == ModuleType::spawn_rate) {
            dials.rate = static_cast<SpawnRateModule*>(module.get());
        } else if (module->type() == ModuleType::initial_lifetime) {
            dials.lifetime = static_cast<InitialLifetimeModule*>(module.get());
        } else if (module->type() == ModuleType::initial_position) {
            dials.position = static_cast<InitialPositionModule*>(module.get());
        }
    }
    float force = 0.0f;
    float drag = 0.0f;
    for (auto& module : emitter.update_modules) {
        if (module->type() == ModuleType::gravity) {
            force = static_cast<GravityModule*>(module.get())->force.y;
        } else if (module->type() == ModuleType::drag) {
            drag = static_cast<DragModule*>(module.get())->drag;
        }
    }
    if (force > 0.0f && drag > 0.0f) dials.rise_speed = force / drag;
    return dials;
}

} // namespace

void EnvironmentSubsystem::update_breathing(const TickContext& ctx) {
    // Breath belongs to play, like the rain: an editor scrub should not
    // wrap the character in bubbles.
    if (!playing() || resources_ == nullptr) {
        stop_breathing(&ctx.scene);
        return;
    }
    const float time = ctx.clock.game_elapsed();

    // Whose head is under which water, and how far.
    struct Under {
        const MotionSource* source;
        glm::vec3 mouth;
        float depth;   // surface above the mouth, metres
    };
    std::vector<Under> under;
    // The water there is, gathered once rather than looked for per source.
    struct Body {
        const WaterComponent* water;
        glm::vec3 centre;
    };
    std::vector<Body> bodies;
    ctx.scene.each<WaterComponent>([&](const Node& node, const WaterComponent& water) {
        if (const auto* spatial = node.as_spatial()) {
            bodies.push_back({&water, glm::vec3(spatial->world_matrix_[3])});
        }
    });
    for (const MotionSource& source : state_.motion_sources) {
        const glm::vec2 xz(source.position.x, source.position.z);
        const glm::vec3 mouth = source.position
            + glm::vec3(0.0f, source.height - BREATH_MOUTH_BELOW_TOP, 0.0f);
        for (const Body& body : bodies) {
            const WaterComponent* water = body.water;
            const glm::vec3 centre = body.centre;
            if (water->extent == WaterComponent::Extent::bounded) {
                const float half = water->size * 0.5f;
                if (std::abs(xz.x - centre.x) > half || std::abs(xz.y - centre.z) > half) continue;
            }
            const float surface = centre.y + water::height_at(water->effective_water, xz, time);
            if (surface <= mouth.y) continue;
            under.push_back({&source, mouth, surface - mouth.y});
            break;
        }
    }

    for (const Under& u : under) {
        auto it = std::find_if(breathing_.begin(), breathing_.end(),
                               [&](const Breathing& b) { return b.source_id == u.source->node_id; });
        const bool fresh = it == breathing_.end();
        if (fresh) {
            auto effect = std::make_shared<VfxEffect>(load_vfx_effect(
                base_path::resolve_shared(std::string(BREATH_EFFECT), ctx.project)));
            if (effect->emitters.empty()) {
                FJELL_CORE_ERROR("Breath: '{}' holds no emitter", BREATH_EFFECT);
                return;
            }
            resolve_vfx_textures(*effect, *resources_, ctx.project);
            auto node = std::make_unique<Node3D>();
            node->name = "Breath";
            auto& emitter = node->components.add<EmitterComponent>();
            emitter.vfx_path = std::string(BREATH_EFFECT);
            emitter.effect = effect;
            // Bursts are fired from here, on the breath's own rhythm.
            emitter.looping = false;
            Breathing started;
            started.source_id = u.source->node_id;
            started.node_id = ctx.scene.add_node(std::move(node));
            started.effect = std::move(effect);
            // Going under lets a breath out at once.
            started.breath_clock = BREATH_PERIOD;
            breathing_.push_back(std::move(started));
            it = std::prev(breathing_.end());
        }
        Breathing& b = *it;
        b.surfaced_for = 0.0f;
        b.breath_clock += ctx.dt;
        const bool exhale_now = b.breath_clock >= BREATH_PERIOD;
        if (exhale_now) b.breath_clock = 0.0f;

        // Every bubble is given the life it needs to reach the surface at
        // its own rise speed, so it pops there rather than hanging under it
        // or sailing through. The stream swells and dies over the exhale;
        // the cluster is a burst fired at its start.
        const float stream = b.breath_clock < BREATH_EXHALE
            ? std::sin(b.breath_clock / BREATH_EXHALE * 3.14159265f) : 0.0f;
        for (VfxEmitter& emitter : b.effect->emitters) {
            BreathDials dials = breath_dials(emitter);
            if (dials.lifetime != nullptr && emitter.name != "pop") {
                const float life = std::max(u.depth, 0.05f) / dials.rise_speed;
                dials.lifetime->lifetime_min = life * 0.9f;
                dials.lifetime->lifetime_max = life * 1.05f;
            }
            if (dials.rate != nullptr) {
                dials.rate->rate = BREATH_STREAM_RATE * stream * u.source->strength;
            }
            // The breath appears around the head, outside it, on a ring the
            // size of the source: the pop's own spread is its own.
            if (dials.position != nullptr && emitter.name != "pop") {
                dials.position->radius =
                    u.source->radius * BREATH_SPREAD_SHARE + BREATH_SPREAD_MARGIN;
            }
        }
        if (auto* node = ctx.scene.find(b.node_id)) {
            if (auto* spatial = node->as_spatial()) spatial->transform.position = u.mouth;
            if (exhale_now) {
                if (auto* emitter = node->components.get<EmitterComponent>()) emitter->fire = true;
            }
        }
    }

    // A head that has surfaced stops breathing out at once and keeps its
    // node for one bubble's life, so what it breathed still reaches the top.
    for (std::size_t i = 0; i < breathing_.size();) {
        Breathing& b = breathing_[i];
        const bool still_under = std::any_of(under.begin(), under.end(), [&](const Under& u) {
            return u.source->node_id == b.source_id;
        });
        if (!still_under) {
            b.surfaced_for += ctx.dt;
            float longest = 0.0f;
            for (VfxEmitter& emitter : b.effect->emitters) {
                BreathDials dials = breath_dials(emitter);
                if (dials.rate != nullptr) dials.rate->rate = 0.0f;
                if (dials.lifetime != nullptr) longest = std::max(longest, dials.lifetime->lifetime_max);
            }
            if (b.surfaced_for > longest) {
                ctx.scene.queue_destroy(b.node_id);
                breathing_.erase(breathing_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        ++i;
    }
}

void EnvironmentSubsystem::stop_breathing(Scene* scene) {
    for (const Breathing& b : breathing_) {
        if (scene != nullptr && b.node_id != 0) scene->queue_destroy(b.node_id);
    }
    breathing_.clear();
}

void EnvironmentSubsystem::update_lightning(const TickContext& ctx) {
    if (!playing()) {
        stop_lightning(&ctx.scene);
        return;
    }

    // Where the listener is, so a strike lands somewhere around them.
    glm::vec3 listener{0.0f};
    if (const auto* cam = ctx.scene.first_node<CameraNode>()) {
        listener = glm::vec3(cam->world_matrix_[3]);
    }

    // A new strike: its own light, its bolt, and thunder on its way.
    if (const auto* strike = scene_weather_.player.strike_this_tick(); strike != nullptr && strike->asset != nullptr) {
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
                vfx_->spawn(*handle, world, ctx.scene, *resources_, ctx.project);
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

void EnvironmentSubsystem::on_play_start(const TickContext& /*ctx*/) {
    editor_weather_ = scene_weather_;
}

void EnvironmentSubsystem::on_play_stop(const TickContext& ctx) {
    end_session_effects(&ctx.scene);
    // Leaving play puts back the weather the editor had, as the play
    // snapshot puts back its scene: what scripts played, the wet ground the
    // session left and the sea it raised were the session's.
    if (editor_weather_.has_value()) {
        scene_weather_ = std::move(*editor_weather_);
        editor_weather_.reset();
    }
}

void EnvironmentSubsystem::on_scene_replaced() {
    end_session_effects(nullptr);
    scene_weather_ = {};
    // The editor's weather is left for a session under way to put back: it
    // goes with the scene that session's snapshot restores.
}

void EnvironmentSubsystem::end_session_effects(Scene* scene) {
    stop_beds(active_beds_);
    stop_beds(outgoing_beds_);
    stop_precipitation(scene);
    stop_lightning(scene);
    stop_breathing(scene);
    phases_seen_ = false;
    fixed_source_positions_.clear();
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
    follow_event(active_beds_, scene_weather_.player.active(), ctx);
    follow_event(outgoing_beds_, scene_weather_.player.outgoing(), ctx);
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
