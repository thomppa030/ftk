// node.hpp — Lightweight node handle for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/component.hpp"
#include "fjell/component_id.hpp"
#include "fjell/transform.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void     fjell_scene_destroy(uint32_t node_id);
FJELL_SDK_EXPORT float*   fjell_scene_get_position(uint32_t node_id);
FJELL_SDK_EXPORT float*   fjell_scene_get_scale(uint32_t node_id);
FJELL_SDK_EXPORT float*   fjell_scene_get_rotation(uint32_t node_id);
FJELL_SDK_EXPORT bool     fjell_scene_get_render_transform(uint32_t node_id, float* position,
                                                           float* rotation, float* scale);
FJELL_SDK_EXPORT void     fjell_scene_teleport(uint32_t node_id, const float* position,
                                               const float* rotation);
FJELL_SDK_EXPORT void*    fjell_scene_get_component(uint32_t node_id, uint32_t type_id);
FJELL_SDK_EXPORT void*    fjell_scene_add_component(uint32_t node_id, uint32_t type_id);
}

namespace fjell {

/// Handle to a node in the scene. Copyable and cheap — it stores only an ID,
/// so hold it by value rather than caching pointers into the scene.
///
/// A handle does not keep its node alive. After the node is destroyed the ID
/// stops resolving: `get_component()` returns null and the transform accessors
/// stop being safe to call. Re-check with `valid()` plus a component lookup if
/// a handle is held across frames.
///
/// @code
/// fjell::Node3D door{fjell::SceneAPI::find_node("Door")};
/// if (door.valid()) {
///     door.position().y += 2.0f;
/// }
/// @endcode
struct Node3D {
    uint32_t node_id{0};

    Node3D() = default;

    /// Wrap a raw node ID — typically from `SceneAPI::find_node()`,
    /// `SceneAPI::spawn()`, or a collision callback's `other_id`.
    explicit Node3D(uint32_t id) : node_id{id} {}

    /// The underlying node ID, for APIs that take one directly
    /// (`Animator`, `MaterialAPI`, `CameraAPI`, ...).
    [[nodiscard]] uint32_t id() const { return node_id; }

    /// Whether this handle holds a non-zero ID. This is a cheap null check,
    /// NOT a liveness check — it stays true after the node is destroyed.
    [[nodiscard]] bool valid() const { return node_id != 0; }

    /// Local position, relative to the parent. Returns a live reference into
    /// the scene, so writes apply immediately and need no dirty-marking.
    ///
    /// @warning Only valid for a live spatial node. Calling this on a
    ///          destroyed node, a zero handle, or a non-spatial node
    ///          dereferences null. Guard with `valid()` when the node's
    ///          lifetime is not obvious from context.
    [[nodiscard]] glm::vec3& position() const {
        return *reinterpret_cast<glm::vec3*>(fjell_scene_get_position(node_id));
    }

    /// Local scale. Same liveness rules as `position()`.
    [[nodiscard]] glm::vec3& scale() const {
        return *reinterpret_cast<glm::vec3*>(fjell_scene_get_scale(node_id));
    }

    /// Local rotation as a quaternion. Same liveness rules as `position()`.
    /// For Euler input use `Transform::set_euler_degrees()`.
    [[nodiscard]] glm::quat& rotation() const {
        return *reinterpret_cast<glm::quat*>(fjell_scene_get_rotation(node_id));
    }

    /// Where the node is drawn, in world space. The simulation moves things in
    /// fixed steps and each frame draws them blended between the last two, so
    /// this is up to a step behind `position()`, which is the simulation's own
    /// state. Follow a node on screen (with a camera, a marker, a trail) by
    /// this, or the follower steps while the node glides. Called from
    /// `tick()`, which runs before the simulation steps, it is where the node
    /// was drawn last frame.
    /// @return The drawn world transform, or an identity transform if the
    ///         node no longer resolves.
    [[nodiscard]] Transform render_transform() const {
        Transform drawn;
        (void)fjell_scene_get_render_transform(node_id, &drawn.position.x, &drawn.rotation.x,
                                               &drawn.scale.x);
        return drawn;
    }

    /// Move the node to a world position and rotation at once: it is drawn
    /// there, not blended there from where it was, and a physics body or
    /// character on it moves with it, keeping its velocity. Writing
    /// `position()` moves a node too, but a body's physics would put it back.
    void teleport(const glm::vec3& world_position, const glm::quat& world_rotation) const {
        fjell_scene_teleport(node_id, &world_position.x, &world_rotation.x);
    }

    /// Look up a component on this node.
    /// @tparam T An engine component (`RigidbodyComponent`, `MeshComponent`,
    ///           ...) or any script component declared with `FJELL_COMPONENT`.
    /// @return Pointer to the component, or null if the node has no such
    ///         component or the ID no longer resolves. Always check.
    /// @code
    /// if (auto* rb = node.get_component<fjell::RigidbodyComponent>()) {
    ///     rb->gravity_enabled = false;
    /// }
    /// @endcode
    template <typename T>
    [[nodiscard]] T* get_component() const {
        return static_cast<T*>(fjell_scene_get_component(node_id, detail::component_type_id<T>()));
    }

    /// Attach a new component to this node at runtime. A node holds one
    /// component of a type: when it already has a T, that one is returned as
    /// it is, and a warning is logged.
    /// @return Pointer to the new component, or null if the node no longer
    ///         resolves, T is an engine component scripts cannot add, or the
    ///         new script's own `begin_play()` took it off again. A script
    ///         component's `begin_play()` runs before this returns when the
    ///         game is already playing.
    template <typename T>
    T* add_component() const {
        return static_cast<T*>(fjell_scene_add_component(node_id, detail::component_type_id<T>()));
    }

    /// Queue this node for destruction along with its children. The node is
    /// removed at the end of the frame, not immediately — it keeps ticking
    /// until then, and other handles to it stay `valid()` for the rest of the
    /// current frame.
    void destroy() const { fjell_scene_destroy(node_id); }
};

// Deferred definition — needs Node3D to be complete
inline Node3D ScriptComponent::self() const { return Node3D{id_}; }

} // namespace fjell
