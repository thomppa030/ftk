#include "scene/component_registry.hpp"
#include "core/base_path.hpp"
#include "ui/game_ui/ui_widget_component.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<UiWidgetComponent>({
        .display_name = "UI Widget",
        .json_key = "ui_widget",
        .color = {0.3f, 0.85f, 0.75f, 1.0f},

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
