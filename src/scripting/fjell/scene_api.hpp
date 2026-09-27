// scene_api.hpp — Static scene API for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/component_id.hpp"
#include "fjell/asset.hpp"
#include "fjell/transform.hpp"

#include <cstdint>

// Engine exports (resolved at dlopen via -rdynamic)
extern "C" {
FJELL_SDK_EXPORT uint32_t fjell_scene_find_node(const char* name);
FJELL_SDK_EXPORT uint32_t fjell_scene_find_node_with(uint32_t type_id);
FJELL_SDK_EXPORT uint32_t fjell_scene_find_child(uint32_t parent_id, const char* name);
FJELL_SDK_EXPORT void     fjell_scene_destroy(uint32_t node_id);
FJELL_SDK_EXPORT float*   fjell_scene_get_position(uint32_t node_id);
FJELL_SDK_EXPORT float*   fjell_scene_get_scale(uint32_t node_id);
FJELL_SDK_EXPORT float*   fjell_scene_get_rotation(uint32_t node_id);
FJELL_SDK_EXPORT void*    fjell_scene_get_component(uint32_t node_id, uint32_t type_id);
FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_empty(const char* name, float x, float y, float z);
FJELL_SDK_EXPORT void*    fjell_scene_add_component(uint32_t node_id, uint32_t type_id);
FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_preset(uint32_t handle_id,
                                                   float x, float y, float z,
                                                   int32_t parent_id);
}

namespace fjell {

class ScriptComponent;
struct Preset;

/// Finding and creating nodes in the running scene.
namespace scene {
/// Find a node by name anywhere in the scene.
/// @param name Node name exactly as shown in the editor hierarchy.
/// @return The node ID, or 0 if no node has that name.
/// @note Linear scan over every node, and the first match wins when names
///       are duplicated. Resolve once in `begin_play()` and cache the ID
///       rather than calling this from `tick()`.
inline uint32_t find_node(const char* name) {
    return fjell_scene_find_node(name);
}

/// Find a direct child of a specific node by name.
/// @param parent_id Node whose children are searched. Only immediate
///                  children are considered, not deeper descendants.
/// @param name      Child name as shown in the hierarchy.
/// @return The child's node ID, or 0 if there is no such child.
/// @note Use this instead of `find_node()` whenever a name repeats across
///       instances — each spawned player has its own "Camera", and only a
///       parent-scoped lookup finds the right one.
/// @code
/// uint32_t cam = fjell::scene::find_child(self().id(), "Camera");
/// @endcode
inline uint32_t find_child(uint32_t parent_id, const char* name) {
    return fjell_scene_find_child(parent_id, name);
}

/// Find a node carrying a component of type T, wherever it sits in the
/// hierarchy and whatever the node happens to be called.
/// @tparam T A script component class or an engine component type.
/// @return The node ID, or 0 if no node in the scene has one. When
///         several do, the one whose T has been in the scene longest.
/// @note Cheap: the engine keeps every T in the scene together and
///       answers from there, without looking at the other nodes. Prefer
///       this over `find_node()` when what you need is the component and
///       the node's name is an accident of how the scene was authored — a
///       rename or a prefab restructure then cannot silently break the
///       lookup.
/// @code
/// fjell::Node3D owner{fjell::scene::find_node_with<PlayerStats>()};
/// if (auto* stats = owner.get_component<PlayerStats>()) { ... }
/// @endcode
template <typename T>
inline uint32_t find_node_with() {
    return fjell_scene_find_node_with(detail::component_type_id<T>());
}

/// Spawn an empty node — no mesh, no components — at a world position.
/// Useful as a marker or as a parent to attach components to.
/// @param name Name for the new node, as it will appear in the hierarchy.
/// @param pos  World position. Defaults to the origin.
/// @return The new node's ID, or 0 if the scene is unavailable.
inline uint32_t spawn(const char* name,
                      const glm::vec3& pos = glm::vec3{0.0f}) {
    return fjell_scene_spawn_empty(name, pos.x, pos.y, pos.z);
}

/// Instantiate a preset (`.fjp`) — a saved node hierarchy with its meshes,
/// components, and property values.
/// @param preset    Preset asset, normally a `[[fj::property]] Asset<Preset>`
///                  field assigned in the inspector.
/// @param pos       World position for the preset's root.
/// @param parent_id Node to parent the spawned root under, or -1 to spawn
///                  at the scene root.
/// @return The root node's ID, or 0 if the preset is unassigned or failed
///         to load.
/// @note Single-player spawn only — it creates the node locally. Use
///       `net::spawn()` for anything that must exist on every peer.
/// @code
/// [[fj::property]] fjell::Asset<fjell::Preset> enemy;
/// // ...
/// uint32_t id = fjell::scene::spawn(enemy, {0.0f, 1.0f, 0.0f});
/// @endcode
inline uint32_t spawn(const Asset<Preset>& preset,
                      const glm::vec3& pos = glm::vec3{0.0f},
                      int32_t parent_id = -1) {
    if (!preset.valid()) return 0;
    return fjell_scene_spawn_preset(preset.handle_id(),
                                    pos.x, pos.y, pos.z, parent_id);
}
} // namespace scene

} // namespace fjell
