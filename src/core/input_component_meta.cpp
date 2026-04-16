#include "scene/component_registry.hpp"
#include "core/input_component.hpp"
#include "scene/node.hpp"
#include "ui/asset_ref_widget.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <string>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<InputComponent>({
        .display_name = "Input",
        .json_key = "input",
        .color = {0.4f, 0.9f, 0.5f, 1.0f},

        .draw = [](Node3D& /*node*/, void* ptr, const InspectorContext& ctx) -> bool {
            auto* ic = static_cast<InputComponent*>(ptr);
            bool changed = false;
            static const std::string ctx_exts[] = {".fjinput"};
            if (asset_ref_field({.label = "Actions Dir", .id = "##InputActionsDir", .extensions = {}, .ctx = &ctx}, ic->actions_dir)) changed = true;
            ImGui::Text("Contexts");
            int remove_idx = -1;
            for (int ci = 0; ci < static_cast<int>(ic->context_paths.size()); ++ci) {
                ImGui::PushID(ci);
                auto label = "##ctx" + std::to_string(ci);
                if (asset_ref_field({.label = "", .id = label.c_str(), .extensions = ctx_exts, .ctx = &ctx}, ic->context_paths[ci])) changed = true;
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) remove_idx = ci;
                ImGui::PopID();
            }
            if (remove_idx >= 0) { ic->context_paths.erase(ic->context_paths.begin() + remove_idx); changed = true; }
            if (ImGui::SmallButton("+ Context")) { ic->context_paths.emplace_back(); changed = true; }
            return changed;
        },

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<InputComponent>().actions_dir = "assets/input/actions";
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out) {
            auto* ic = static_cast<const InputComponent*>(ptr);
            out["contexts"] = ic->context_paths;
            if (!ic->actions_dir.empty()) out["actions_dir"] = ic->actions_dir;
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& ic = node.components.add<InputComponent>();
            if (j.contains("contexts") && j["contexts"].is_array()) {
                for (const auto& ctx : j["contexts"]) ic.context_paths.push_back(ctx.get<std::string>());
            }
            ic.actions_dir = j.value("actions_dir", "");
        },

        .script_type_name = {},
    });
    return true;
}();

} // namespace fjell
