// node.hpp — Node3D, a node in the scene as a script sees it
#pragma once

#include "fjell/api.h"
#include "fjell/component.hpp"
#include "fjell/component_id.hpp"
#include "fjell/ref.hpp"
#include "fjell/transform.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT float*   fjell_node_position(void* node);
FJELL_SDK_EXPORT float*   fjell_node_rotation(void* node);
FJELL_SDK_EXPORT float*   fjell_node_scale(void* node);
FJELL_SDK_EXPORT void     fjell_node_render_transform(const void* node, float* position, float* rotation,
                                                      float* scale);
FJELL_SDK_EXPORT void     fjell_node_teleport(void* node, const float* position, const float* rotation);
FJELL_SDK_EXPORT void     fjell_node_add_component(void* node, uint32_t type_id);
FJELL_SDK_EXPORT uint32_t fjell_node_find_child(const void* node, const char* name);
FJELL_SDK_EXPORT void     fjell_node_destroy(const void* node);
}

namespace fjell {

/// A node in the scene, as a script sees it: the node itself, not a copy of
/// it and not an id. A script reaches its own node with `self()` and any other
/// through a `Ref<Node3D>`, whose `->` stops the game when that node is gone;
/// nothing else hands one out, so a `Node3D` in front of a script is there.
///
/// @code
/// [[fj::property]] fjell::Ref<fjell::Node3D> door;
///
/// void open() {
///     door->position().y += 2.0f;
///     self().find_child("Lamp")->destroy();
/// }
/// @endcode
struct Node3D {
    Node3D() = delete;
    Node3D(const Node3D&) = delete;
    Node3D& operator=(const Node3D&) = delete;

    /// Local position, relative to the parent: the node's own, so writes
    /// apply at once.
    [[nodiscard]] glm::vec3& position() { return *reinterpret_cast<glm::vec3*>(fjell_node_position(this)); }

    /// Local rotation, relative to the parent. For Euler input use
    /// `Transform::set_euler_degrees()`.
    [[nodiscard]] glm::quat& rotation() { return *reinterpret_cast<glm::quat*>(fjell_node_rotation(this)); }

    /// Local scale.
    [[nodiscard]] glm::vec3& scale() { return *reinterpret_cast<glm::vec3*>(fjell_node_scale(this)); }

    /// Where the node is drawn, in world space. The simulation moves things in
    /// fixed steps and each frame draws them blended between the last two, so
    /// this is up to a step behind `position()`, which is the simulation's own
    /// state. Follow a node on screen (with a camera, a marker, a trail) by
    /// this, or the follower steps while the node glides. Called from
    /// `tick()`, which runs before the simulation steps, it is where the node
    /// was drawn last frame.
    [[nodiscard]] Transform render_transform() const {
        Transform drawn;
        fjell_node_render_transform(this, &drawn.position.x, &drawn.rotation.x, &drawn.scale.x);
        return drawn;
    }

    /// Move the node to a world position and rotation at once: it is drawn
    /// there, not blended there from where it was, and a physics body or
    /// character on it moves with it, keeping its velocity. Writing
    /// `position()` moves a node too, but a body's physics would put it back.
    void teleport(const glm::vec3& world_position, const glm::quat& world_rotation) {
        fjell_node_teleport(this, &world_position.x, &world_rotation.x);
    }

    /// The node's component of type T, as a Ref: it reads as not there when
    /// the node has none, and as gone once the node or the component is.
    /// @tparam T An engine component (`RigidbodyComponent`, `MeshComponent`,
    ///           ...) or any script component declared with `FJELL_COMPONENT`.
    /// @code
    /// if (auto body = self().get_component<fjell::RigidbodyComponent>()) {
    ///     body->gravity_enabled = false;
    /// }
    /// @endcode
    template <typename T>
    [[nodiscard]] Ref<T> get_component() const {
        return Ref<T>{*this};
    }

    /// Attach a component of type T at runtime and refer to it. A node holds
    /// one component of a type: when it already has a T, that one is kept as
    /// it is, and a warning is logged. The Ref reads as not there when T is an
    /// engine component scripts cannot add, or a new script's own
    /// `begin_play()`, which runs before this returns while the game plays,
    /// took it off again.
    template <typename T>
    Ref<T> add_component() {
        fjell_node_add_component(this, detail::component_type_id<T>());
        return Ref<T>{*this};
    }

    /// The direct child called `name`, or an empty Ref when there is none.
    /// Only immediate children are searched. Names repeat across instances,
    /// each spawned player has its own "Camera", and this finds the one under
    /// this node.
    [[nodiscard]] Ref<Node3D> find_child(const char* name) const {
        return Ref<Node3D>{fjell_node_find_child(this, name)};
    }

    /// Queue this node for destruction along with its children. It is removed
    /// at the end of the frame, not at once: it keeps ticking until then, and
    /// Refs to it find it for the rest of the frame.
    void destroy() { fjell_node_destroy(this); }
};

// Deferred definition — needs Node3D to be declared
inline Node3D& ScriptComponent::self() const { return *node_; }

} // namespace fjell
