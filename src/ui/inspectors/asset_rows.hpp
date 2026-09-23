#pragma once

#include "core/base_path.hpp"
#include "core/project_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/component_meta.hpp"

#include <string>

// Pieces the component inspectors share for the assets they hold.

namespace fjell {

/// The material name a mesh holds when it has none of its own.
inline constexpr const char* DEFAULT_MATERIAL = "default";

/// The project the inspector is editing, or null.
[[nodiscard]] inline const Project* project_of(const InspectorContext& ctx) {
    return ctx.project_mgr != nullptr ? ctx.project_mgr->active() : nullptr;
}

/// Loads `name` (a .fjmat reference, or the default) into a material handle
/// and pointer, the way every mesh-like component holds its material.
template <typename Handle, typename Pointer>
void load_material(const std::string& name, Handle& handle, Pointer& material,
                   const InspectorContext& ctx) {
    if (ctx.resource_manager == nullptr) return;
    if (name != DEFAULT_MATERIAL) {
        if (auto result = ctx.resource_manager->load_material_asset(
                base_path::resolve_asset(name, project_of(ctx)), project_of(ctx))) {
            handle = *result;
            material = ctx.resource_manager->get(handle);
        }
        return;
    }
    handle = ctx.resource_manager->create_material(DEFAULT_MATERIAL,
                                                   ctx.resource_manager->default_white_handle());
    material = ctx.resource_manager->get(handle);
}

/// The Material row of a mesh-like component: the default material shows as
/// an empty slot, and clearing the slot goes back to it. Returns true when
/// `name` changed; the caller loads it.
bool material_row(const char* id, std::string& name, const InspectorContext& ctx);

} // namespace fjell
