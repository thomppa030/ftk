#include "scene/component_registry.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<SpriteComponent>({
        .display_name = "Sprite",
        .json_key = "sprite",
        .color = {0.40f, 0.75f, 0.40f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& ctx) -> bool {
            auto* sp = static_cast<SpriteComponent*>(ptr);
            bool changed = false;
            static const std::string exts[] = {".png", ".jpg", ".jpeg"};
            if (asset_ref_field({.label = "Texture", .id = "##SpriteTexture", .extensions = exts, .ctx = &ctx}, sp->texture_path)) { sp->texture_loaded = false; changed = true; }
            ImGui::Text("Tint"); ImGui::SetNextItemWidth(-1);
            if (ImGui::ColorEdit4("##SpriteTint", &sp->tint.r)) changed = true;
            ImGui::Text("Opacity"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##SpriteOpacity", &sp->opacity, 0.01f, 0.0f, 1.0f)) changed = true;
            if (ImGui::Checkbox("Flat (2D)", &sp->flat)) changed = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Flip H", &sp->flip_h)) changed = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Pixel Art", &sp->pixel_art)) { sp->texture_loaded = false; changed = true; }
            ImGui::Text("Pixels Per Unit"); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##SpritePPU", &sp->pixels_per_unit, 1.0f, 0.0f, 1000.0f, "%.0f")) changed = true;
            ImGui::Text("Sort Layer / Order");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
            if (ImGui::DragInt("##SpriteLayer", &sp->sort_layer, 1.0f)) changed = true;
            ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
            if (ImGui::DragInt("##SpriteOrder", &sp->sort_order, 1.0f)) changed = true;
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<SpriteComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* sp = static_cast<const SpriteComponent*>(ptr);
            out["texture"] = sp->texture_path;
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
        },

        .script_type_name = "SpriteComponent",
        .script_get = [](Node& node) -> void* { return node.components.get<SpriteComponent>(); },
        .script_add = [](Node& node) -> void* { return &node.components.add<SpriteComponent>(); },
    });
    return true;
}();

} // namespace fjell
