#include "scene/component_registry.hpp"
#include "core/input_component.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<InputComponent>({
        .display_name = "Input",
        .json_key = "input",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<InputComponent>().actions_dir = "assets/input/actions";
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
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
