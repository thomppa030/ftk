#include "scene/component_registry.hpp"
#include "core/base_path.hpp"
#include "ui/game_ui/ui_widget_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<UiWidgetComponent>({
        .display_name = "UI Widget",
        .json_key = "ui_widget",
        .color = {0.3f, 0.85f, 0.75f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& ctx) -> bool {
            auto* wc = static_cast<UiWidgetComponent*>(ptr);
            bool changed = false;
            static const std::string exts[] = {".fjwidget"};
            if (asset_ref_field({.label = "Widget", .id = "##UiWidgetPath", .extensions = exts, .ctx = &ctx}, wc->widget_path)) changed = true;
            int mode = static_cast<int>(wc->render_mode);
            const char* modes[] = {"Screen Space", "World Space"};
            if (ImGui::Combo("Render Mode", &mode, modes, 2)) { wc->render_mode = static_cast<UiWidgetComponent::RenderMode>(mode); changed = true; }

            // Space-separated, the way a class="..." attribute reads. The
            // buffer is refilled from the component whenever the field is
            // not being typed into, so undo and external edits show up, and
            // the value commits on Enter or focus loss like every other
            // text field, so the split never runs under the cursor.
            static char class_buf[256];
            static const void* class_buf_owner = nullptr;
            bool refill = class_buf_owner != ptr || !ImGui::IsAnyItemActive();
            if (refill) {
                std::string joined;
                for (const auto& cls : wc->style_classes) {
                    if (!joined.empty()) joined += ' ';
                    joined += cls;
                }
                std::snprintf(class_buf, sizeof(class_buf), "%s", joined.c_str());
                class_buf_owner = ptr;
            }
            ImGui::InputText("Style Classes", class_buf, sizeof(class_buf));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                wc->style_classes.clear();
                std::string token;
                for (const char* c = class_buf;; ++c) {
                    if (*c == ' ' || *c == '\0') {
                        if (!token.empty()) wc->style_classes.push_back(token);
                        token.clear();
                        if (*c == '\0') break;
                    } else {
                        token += *c;
                    }
                }
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Classes added to the widget's root node for this placement");
            }
            if (wc->render_mode == UiWidgetComponent::RenderMode::screen_space) {
                using Scaling = UiWidgetComponent::Scaling;
                int scaling = static_cast<int>(wc->scaling);
                const char* scalings[] = {"Design resolution", "Native pixels", "Custom height"};
                if (ImGui::Combo("Scaling", &scaling, scalings, 3)) { wc->scaling = static_cast<Scaling>(scaling); changed = true; }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Design resolution: scale so the project's design height fills the viewport.\n"
                                      "Native pixels: one layout pixel per screen pixel.\n"
                                      "Custom height: scale so this many layout pixels fill the viewport.");
                }
                if (wc->scaling == Scaling::custom) {
                    if (ImGui::DragFloat("Reference Height", &wc->custom_reference_height, 1.0f, 240.0f, 4320.0f, "%.0f")) changed = true;
                }
            }
            if (wc->render_mode == UiWidgetComponent::RenderMode::world_space) {
                if (ImGui::DragFloat("Pixels/Unit", &wc->pixels_per_unit, 1.0f, 1.0f, 1000.0f)) changed = true;
                if (ImGui::Checkbox("Billboard", &wc->billboard)) changed = true;
                if (ImGui::DragFloat2("Virtual Viewport", &wc->virtual_viewport.x, 1.0f, 1.0f, 2048.0f)) changed = true;
                if (ImGui::DragFloat3("Offset", &wc->offset.x, 0.05f)) changed = true;
            }
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<UiWidgetComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* wc = static_cast<const UiWidgetComponent*>(ptr);
            out["widget"] = base_path::asset_reference(wc->widget_path, proj);
            if (wc->render_mode != UiWidgetComponent::RenderMode::screen_space)
                out["render_mode"] = static_cast<int>(wc->render_mode);
            if (!wc->style_classes.empty()) out["style_classes"] = wc->style_classes;
            if (wc->scaling != UiWidgetComponent::Scaling::design_resolution)
                out["scaling"] = static_cast<int>(wc->scaling);
            if (wc->scaling == UiWidgetComponent::Scaling::custom)
                out["reference_height"] = wc->custom_reference_height;
            if (wc->render_mode == UiWidgetComponent::RenderMode::world_space) {
                out["pixels_per_unit"] = wc->pixels_per_unit;
                out["billboard"] = wc->billboard;
                out["virtual_viewport"] = {wc->virtual_viewport.x, wc->virtual_viewport.y};
                out["offset"] = {wc->offset.x, wc->offset.y, wc->offset.z};
            }
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& wc = node.components.add<UiWidgetComponent>();
            wc.widget_path = j.value("widget", "");
            wc.render_mode = static_cast<UiWidgetComponent::RenderMode>(j.value("render_mode", 0));
            if (j.contains("style_classes") && j["style_classes"].is_array()) {
                for (const auto& cls : j["style_classes"]) wc.style_classes.push_back(cls.get<std::string>());
            }
            wc.scaling = static_cast<UiWidgetComponent::Scaling>(j.value("scaling", 0));
            wc.custom_reference_height = j.value("reference_height", 1080.0f);
            wc.pixels_per_unit = j.value("pixels_per_unit", 100.0f);
            wc.billboard = j.value("billboard", true);
            if (j.contains("virtual_viewport")) { auto& vv = j["virtual_viewport"]; wc.virtual_viewport = {vv[0].get<float>(), vv[1].get<float>()}; }
            if (j.contains("offset")) { auto& off = j["offset"]; wc.offset = {off[0].get<float>(), off[1].get<float>(), off[2].get<float>()}; }
        },

        .script_type_name = {},
    });
    return true;
}();

} // namespace fjell
