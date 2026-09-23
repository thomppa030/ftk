#include "scene/component_registry.hpp"
#include "renderer/components/arrow_component.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

static const bool registered = [] {
    ComponentRegistry::instance().register_component<ArrowComponent>({
        .display_name = "Arrow",
        .json_key = "arrow",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            node.components.add<ArrowComponent>();
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* /*proj*/) {
            auto* arr = static_cast<const ArrowComponent*>(ptr);
            out["length"] = arr->length;
            out["color"] = {arr->color.x, arr->color.y, arr->color.z};
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& /*res*/, const Project* /*proj*/) {
            auto& arr = node.components.add<ArrowComponent>();
            arr.length = j.value("length", 2.0f);
            if (j.contains("color")) {
                auto& c = j["color"];
                arr.color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
            }
        },

        .script_type_name = {},
    });
    return true;
}();

} // namespace fjell
