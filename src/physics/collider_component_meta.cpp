#include "scene/component_registry.hpp"
#include "core/base_path.hpp"
#include "ftk/base/log.hpp"
#include "physics/physics_types.hpp"
#include "physics/collider_surface.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/node.hpp"

#include <nlohmann/json.hpp>

namespace fjell {

void load_collider_surface(ColliderComponent& col, ResourceManager* resources,
                           const Project* project) {
    col.surface = 0;
    if (col.surface_path.empty() || resources == nullptr) return;
    auto handle = resources->load_surface_asset(base_path::resolve_asset(col.surface_path, project));
    if (handle) col.surface = handle->id;
    else FJELL_CORE_ERROR("collider surface: {}", handle.error());
}

static const bool registered = [] {
    ComponentRegistry::instance().register_component<ColliderComponent>({
        .display_name = "Collider",
        .json_key = "collider",

        .init = [](Node3D& node, const InspectorContext& /*ctx*/) {
            auto& col = node.components.add<ColliderComponent>();
            // A collider added to a primitive should already match it; anything
            // else gets a unit box to be dragged into shape.
            if (!fit_collider_to_primitive(col, node.source)) {
                col.shape = ColliderShape::Box;
            }
        },

        .serialize = [](const void* ptr, const Node& /*node*/, nlohmann::json& out,
                        const Project* proj) {
            auto* col = static_cast<const ColliderComponent*>(ptr);
            out["shape"] = static_cast<int>(col->shape);
            out["inherit_from_mesh"] = col->inherit_from_mesh;
            out["half_extents"] = {col->half_extents.x, col->half_extents.y, col->half_extents.z};
            out["radius"] = col->radius;
            out["height"] = col->height;
            out["offset"] = {col->offset.x, col->offset.y, col->offset.z};
            out["is_trigger"] = col->is_trigger;
            out["channel"] = collision_channel_name(col->filter.channel);
            out["block_mask"] = col->filter.block_mask;
            out["overlap_mask"] = col->filter.overlap_mask;
            if (!col->surface_path.empty()) {
                out["surface"] = base_path::asset_reference(col->surface_path, proj);
            }
        },

        .deserialize = [](Node& node, const nlohmann::json& j, ResourceManager& res, const Project* proj) {
            auto& col = node.components.add<ColliderComponent>();
            col.shape = static_cast<ColliderShape>(j.value("shape", 0));
            col.inherit_from_mesh = j.value("inherit_from_mesh", false);
            if (j.contains("half_extents")) { auto& he = j["half_extents"]; col.half_extents = {he[0].get<float>(), he[1].get<float>(), he[2].get<float>()}; }
            col.radius = j.value("radius", 0.5f);
            col.height = j.value("height", 1.0f);
            if (j.contains("offset")) { auto& o = j["offset"]; col.offset = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()}; }
            col.is_trigger = j.value("is_trigger", false);

            // Stored by name, so adding or reordering channels cannot silently
            // repoint every collider in every scene.
            col.filter.channel = CollisionChannel::world_static;
            collision_channel_from_name(j.value("channel", std::string{}), col.filter.channel);
            // A scene saved before channels existed gets the defaults for its
            // channel rather than an all-zero table that collides with nothing.
            auto defaults = default_filter_for(col.filter.channel);
            col.filter.block_mask = j.value("block_mask", defaults.block_mask);
            col.filter.overlap_mask = j.value("overlap_mask", defaults.overlap_mask);

            col.surface_path = j.value("surface", std::string{});
            load_collider_surface(col, &res, proj);
        },

        .script_type_name = "ColliderComponent",
        .script_add = [](Node& node) -> void* { return &node.components.add<ColliderComponent>(); },
    });
    return true;
}();

} // namespace fjell
