#include "scene/component_registry.hpp"
#include "core/base_path.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<SpriteComponent>({
        .display_name = "Sprite",
        .json_key = "sprite",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<SpriteComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* sp = static_cast<const SpriteComponent*>(ptr);
            out["texture"] = base_path::asset_reference(sp->texture_path, proj);
            out["tint"] = {sp->tint.r, sp->tint.g, sp->tint.b, sp->tint.a};
            out["opacity"] = sp->opacity;
            out["editor_only"] = sp->editor_only;
            if (sp->uv_rect != glm::vec4(0, 0, 1, 1)) out["uv_rect"] = {sp->uv_rect.x, sp->uv_rect.y, sp->uv_rect.z, sp->uv_rect.w};
            if (sp->flip_h) out["flip_h"] = true;
            if (sp->flat) out["flat"] = true;
            if (sp->pixel_art) out["pixel_art"] = true;
            if (sp->pixels_per_unit > 0.0f) out["pixels_per_unit"] = sp->pixels_per_unit;
            if (sp->sort_layer != 0) out["sort_layer"] = sp->sort_layer;
            if (sp->sort_order != 0) out["sort_order"] = sp->sort_order;
            if (sp->lit) out["lit"] = true;
            if (!sp->normal_path.empty()) out["normal"] = base_path::asset_reference(sp->normal_path, proj);
            if (sp->roughness != 1.0f) out["roughness"] = sp->roughness;
            if (sp->metallic != 0.0f) out["metallic"] = sp->metallic;
            if (sp->emission != 0.0f) out["emission"] = sp->emission;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& sp = node.components.add<SpriteComponent>();
            sp.texture_path = j.value("texture", "");
            if (sp.texture_path.starts_with("assets/icons/")) sp.texture_path = "engine_assets/icons/" + sp.texture_path.substr(13);
            if (j.contains("tint")) { auto& t = j["tint"]; sp.tint = {t[0].get<float>(), t[1].get<float>(), t[2].get<float>(), t[3].get<float>()}; }
            if (j.contains("uv_rect")) { auto& u = j["uv_rect"]; sp.uv_rect = {u[0].get<float>(), u[1].get<float>(), u[2].get<float>(), u[3].get<float>()}; }
            sp.opacity = j.value("opacity", 1.0f);
            sp.editor_only = j.value("editor_only", false);
            sp.flip_h = j.value("flip_h", false);
            sp.flat = j.value("flat", false);
            sp.pixel_art = j.value("pixel_art", false);
            sp.pixels_per_unit = j.value("pixels_per_unit", 0.0f);
            sp.sort_layer = j.value("sort_layer", 0);
            sp.sort_order = j.value("sort_order", 0);
            sp.lit = j.value("lit", false);
            sp.normal_path = j.value("normal", "");
            sp.roughness = j.value("roughness", 1.0f);
            sp.metallic = j.value("metallic", 0.0f);
            sp.emission = j.value("emission", 0.0f);
        },

        .script_type_name = "SpriteComponent",
        .script_add = [](Node& node) -> void* { return &node.components.add<SpriteComponent>(); },
    });
    return true;
}();

} // namespace fjell
