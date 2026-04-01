#include "scene/component_registry.hpp"
#include "renderer/particle_emitter.hpp"
#include "scene/node.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<EmitterComponent>({
        .display_name = "Particle Emitter",
        .json_key = "emitter",
        .color = {0.78f, 0.55f, 0.85f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& /*ctx*/) -> bool {
            auto* em = static_cast<EmitterComponent*>(ptr);
            bool changed = false;
            if (ImGui::DragFloat("Emit Rate", &em->emission_rate, 1.0f, 0.0f, 10000.0f)) changed = true;
            if (ImGui::DragFloat("Lifetime Min", &em->lifetime_min, 0.1f, 0.01f, 30.0f)) changed = true;
            if (ImGui::DragFloat("Lifetime Max", &em->lifetime_max, 0.1f, 0.01f, 30.0f)) changed = true;
            int max_p = static_cast<int>(em->max_particles);
            if (ImGui::DragInt("Max Particles", &max_p, 100, 100, 100000)) { em->max_particles = static_cast<uint32_t>(max_p); changed = true; }
            if (ImGui::DragFloat3("Velocity Min", &em->velocity_min.x, 0.1f)) changed = true;
            if (ImGui::DragFloat3("Velocity Max", &em->velocity_max.x, 0.1f)) changed = true;
            if (ImGui::DragFloat3("Gravity", &em->gravity.x, 0.1f)) changed = true;
            if (ImGui::ColorEdit4("Color Start", &em->color_start.r)) changed = true;
            if (ImGui::ColorEdit4("Color End", &em->color_end.r)) changed = true;
            if (ImGui::DragFloat("Size Start", &em->size_start, 0.01f, 0.001f, 10.0f)) changed = true;
            if (ImGui::DragFloat("Size End", &em->size_end, 0.01f, 0.001f, 10.0f)) changed = true;
            static const char* blend_labels[] = {"Additive", "Alpha"};
            int bm = static_cast<int>(em->blend_mode);
            if (ImGui::Combo("Blend Mode", &bm, blend_labels, 2)) { em->blend_mode = static_cast<ParticleBlendMode>(bm); changed = true; }
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<EmitterComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* em = static_cast<const EmitterComponent*>(ptr);
            if (!em->vfx_path.empty()) {
                // VFX path is handled separately — store in node-level JSON, not component JSON
                // The serializer will check for this
                out["_vfx_path"] = em->vfx_path;
            } else {
                out["emission_rate"] = em->emission_rate;
                out["lifetime_min"] = em->lifetime_min;
                out["lifetime_max"] = em->lifetime_max;
                out["max_particles"] = em->max_particles;
                out["velocity_min"] = {em->velocity_min.x, em->velocity_min.y, em->velocity_min.z};
                out["velocity_max"] = {em->velocity_max.x, em->velocity_max.y, em->velocity_max.z};
                out["gravity"] = {em->gravity.x, em->gravity.y, em->gravity.z};
                out["color_start"] = {em->color_start.r, em->color_start.g, em->color_start.b, em->color_start.a};
                out["color_end"] = {em->color_end.r, em->color_end.g, em->color_end.b, em->color_end.a};
                out["size_start"] = em->size_start;
                out["size_end"] = em->size_end;
                out["blend_mode"] = static_cast<int>(em->blend_mode);
            }
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& em = node.components.add<EmitterComponent>();
            em.emission_rate = j.value("emission_rate", 50.0f);
            em.lifetime_min = j.value("lifetime_min", 1.0f);
            em.lifetime_max = j.value("lifetime_max", 3.0f);
            em.max_particles = j.value("max_particles", 10000u);
            if (j.contains("velocity_min")) { auto& v = j["velocity_min"]; em.velocity_min = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()}; }
            if (j.contains("velocity_max")) { auto& v = j["velocity_max"]; em.velocity_max = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()}; }
            if (j.contains("gravity")) { auto& g = j["gravity"]; em.gravity = {g[0].get<float>(), g[1].get<float>(), g[2].get<float>()}; }
            if (j.contains("color_start")) { auto& c = j["color_start"]; em.color_start = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()}; }
            if (j.contains("color_end")) { auto& c = j["color_end"]; em.color_end = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()}; }
            em.size_start = j.value("size_start", 0.1f);
            em.size_end = j.value("size_end", 0.02f);
            em.blend_mode = static_cast<ParticleBlendMode>(j.value("blend_mode", 0));
        },
    });
    return true;
}();

} // namespace fjell
