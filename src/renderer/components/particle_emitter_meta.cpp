#include "scene/component_registry.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/systems/vfx_asset.hpp"
#include "renderer/systems/vfx_emitter.hpp"
#include "renderer/systems/vfx_module.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

// Build a VfxEffect from legacy flat JSON fields (v1 scene compat)
static std::shared_ptr<VfxEffect> effect_from_legacy_json(const nlohmann::json& j) {
    auto effect = std::make_shared<VfxEffect>();
    effect->name = "effect";

    VfxEmitter em;
    em.max_particles = j.value("max_particles", 10000u);
    em.blend_mode = static_cast<ParticleBlendMode>(j.value("blend_mode", 0));

    auto rate = std::make_unique<SpawnRateModule>();
    rate->rate = j.value("emission_rate", 50.0f);
    em.spawn_modules.push_back(std::move(rate));

    auto lt = std::make_unique<InitialLifetimeModule>();
    lt->lifetime_min = j.value("lifetime_min", 1.0f);
    lt->lifetime_max = j.value("lifetime_max", 3.0f);
    em.spawn_modules.push_back(std::move(lt));

    auto pos = std::make_unique<InitialPositionModule>();
    em.spawn_modules.push_back(std::move(pos));

    auto vel = std::make_unique<InitialVelocityModule>();
    vel->mode = VelocityMode::random;
    if (j.contains("velocity_min")) {
        auto& v = j["velocity_min"];
        vel->velocity_min = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
    }
    if (j.contains("velocity_max")) {
        auto& v = j["velocity_max"];
        vel->velocity_max = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
    }
    em.spawn_modules.push_back(std::move(vel));

    auto col = std::make_unique<InitialColorModule>();
    if (j.contains("color_start")) {
        auto& c = j["color_start"];
        col->color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    em.spawn_modules.push_back(std::move(col));

    auto sz = std::make_unique<InitialSizeModule>();
    sz->size_min = j.value("size_start", 0.1f);
    sz->size_max = j.value("size_start", 0.1f);
    em.spawn_modules.push_back(std::move(sz));

    auto grav = std::make_unique<GravityModule>();
    if (j.contains("gravity")) {
        auto& g = j["gravity"];
        grav->force = {g[0].get<float>(), g[1].get<float>(), g[2].get<float>()};
    }
    em.update_modules.push_back(std::move(grav));

    auto col_life = std::make_unique<ColorOverLifeModule>();
    if (j.contains("color_start")) {
        auto& c = j["color_start"];
        col_life->color_start = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("color_end")) {
        auto& c = j["color_end"];
        col_life->color_end = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    em.update_modules.push_back(std::move(col_life));

    auto sz_life = std::make_unique<SizeOverLifeModule>();
    sz_life->size_start = j.value("size_start", 0.1f);
    sz_life->size_end = j.value("size_end", 0.02f);
    em.update_modules.push_back(std::move(sz_life));

    em.render_modules.push_back(std::make_unique<BillboardModule>());

    effect->emitters.push_back(std::move(em));
    return effect;
}

void load_emitter_effect(EmitterComponent& emitter, ResourceManager& resources, const Project* project) {
    emitter.effect.reset();
    if (emitter.vfx_path.empty()) return;
    try {
        auto effect = load_vfx_effect(base_path::resolve_asset(emitter.vfx_path, project));
        resolve_vfx_textures(effect, resources, project);
        emitter.effect = std::make_shared<VfxEffect>(std::move(effect));
    } catch (const std::exception& e) {
        FJELL_CORE_ERROR("Failed to load VFX '{}': {}", emitter.vfx_path, e.what());
    }
}

static const bool registered = [] {
    ComponentRegistry::instance().register_component<EmitterComponent>({
        .display_name = "Particle Emitter",
        .json_key = "emitter",
        .color = {0.78f, 0.55f, 0.85f, 1.0f},

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<EmitterComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* em = static_cast<const EmitterComponent*>(ptr);
            if (!em->vfx_path.empty()) {
                out["_vfx_path"] = em->vfx_path;
            }
            if (!em->looping) {
                out["looping"] = false;
            }
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& em = node.components.add<EmitterComponent>();
            em.looping = j.value("looping", true);
            // Legacy v1 flat fields — convert to module-based effect
            if (j.contains("emission_rate")) {
                em.effect = effect_from_legacy_json(j);
            }
        },

        .script_type_name = {},
    });
    return true;
}();

} // namespace fjell
