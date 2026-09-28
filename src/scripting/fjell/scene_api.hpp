// scene_api.hpp — Finding and creating nodes, for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/component_id.hpp"
#include "fjell/asset.hpp"
#include "fjell/node.hpp"
#include "fjell/ref.hpp"
#include "fjell/transform.hpp"

#include <cstdint>

// Engine exports (resolved at dlopen via -rdynamic)
extern "C" {
FJELL_SDK_EXPORT uint32_t fjell_scene_find_node(const char* name);
FJELL_SDK_EXPORT uint32_t fjell_scene_find(uint32_t type_id);
FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_empty(const char* name, float x, float y, float z);
FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_preset(uint32_t handle_id,
                                                   float x, float y, float z,
                                                   uint32_t parent_id);
}

namespace fjell {

struct Preset;

/// Finding and creating nodes in the running scene.
namespace scene {
/// A node by name, anywhere in the scene.
/// @param name Node name exactly as shown in the editor hierarchy.
/// @return The node, or an empty Ref when no node has that name.
/// @note Linear scan over every node, and the first match wins when names
///       are duplicated. A name is an accident of how the scene was
///       authored: prefer a `[[fj::property]] Ref<Node3D>` picked in the
///       inspector, or `find<T>()` for the component you are after.
inline Ref<Node3D> find_node(const char* name) {
    return Ref<Node3D>{fjell_scene_find_node(name)};
}

/// The component of type T, on whichever node carries one, wherever it sits
/// in the hierarchy and whatever the node is called.
/// @tparam T A script component class or an engine component type.
/// @return The T, or an empty Ref when no node in the scene has one. When
///         several do, the one that has been in the scene longest.
/// @note Cheap: the engine keeps every T in the scene together and answers
///       from there, without looking at the other nodes. Its node is
///       `.node()`.
/// @code
/// auto stats = fjell::scene::find<PlayerStats>();
/// if (stats) stats->hp = stats->max_hp;
/// @endcode
template <typename T>
inline Ref<T> find() {
    return Ref<T>{fjell_scene_find(detail::component_type_id<T>())};
}

/// Spawn an empty node — no mesh, no components — at a world position.
/// Useful as a marker or as a parent to attach components to.
/// @param name Name for the new node, as it will appear in the hierarchy.
/// @param pos  World position. Defaults to the origin.
/// @return The new node, usable at once, or an empty Ref when no scene is
///         loaded.
inline Ref<Node3D> spawn(const char* name, const glm::vec3& pos = glm::vec3{0.0f}) {
    return Ref<Node3D>{fjell_scene_spawn_empty(name, pos.x, pos.y, pos.z)};
}

/// Instantiate a preset (`.fjp`) — a saved node hierarchy with its meshes,
/// components, and property values — at the root of the scene.
/// @param preset Preset asset, normally a `[[fj::property]] Asset<Preset>`
///               field assigned in the inspector. One that is unset or
///               failed to load stops the game (see `Asset`).
/// @param pos    World position for the preset's root.
/// @return The preset's root node, usable at once, or an empty Ref when no
///         scene is loaded.
/// @note Single-player spawn only — it creates the node locally. Use
///       `net::spawn()` for anything that must exist on every peer.
/// @code
/// [[fj::property]] fjell::Asset<fjell::Preset> enemy;
/// // ...
/// auto spawned = fjell::scene::spawn(enemy, {0.0f, 1.0f, 0.0f});
/// spawned->find_child("Weapon")->destroy();
/// @endcode
inline Ref<Node3D> spawn(const Asset<Preset>& preset, const glm::vec3& pos = glm::vec3{0.0f}) {
    detail::require(preset);
    return Ref<Node3D>{fjell_scene_spawn_preset(preset.handle_id(), pos.x, pos.y, pos.z, 0)};
}

/// Instantiate a preset under a parent, which it then moves with. `pos` is
/// still in world space. A parent that is gone stops the game (see `Ref`);
/// `self()` passes the script's own node.
inline Ref<Node3D> spawn(const Asset<Preset>& preset, const glm::vec3& pos, const Ref<Node3D>& parent) {
    detail::require(preset);
    detail::require(parent);
    return Ref<Node3D>{fjell_scene_spawn_preset(preset.handle_id(), pos.x, pos.y, pos.z, parent.node_id())};
}
} // namespace scene

} // namespace fjell
