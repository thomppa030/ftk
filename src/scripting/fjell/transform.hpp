// fjell_transform.hpp — Transform struct for Fjell game code
// Shipped to game projects alongside fjell_api.h.
// Uses shared include guard with scene/transform.hpp to prevent ODR issues.

#ifndef FJELL_TRANSFORM_HPP
#define FJELL_TRANSFORM_HPP

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace fjell {

/// Position, rotation, and scale of a node, always relative to its parent.
/// Reached through `ScriptComponent::transform()` or `Node3D`'s accessors.
///
/// The engine detects changes by comparing values each frame, so writing to
/// these fields is enough — there is no dirty flag to set.
struct Transform {
    /// Local position, in parent space.
    glm::vec3 position{0.0f};

    /// Local rotation as a quaternion. Use `set_euler_degrees()` for
    /// human-readable angles, or `glm::rotate()`/`glm::slerp()` to compose.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity quaternion

    /// Local scale per axis. 1.0 is unscaled; non-uniform values distort
    /// child nodes and physics shapes.
    glm::vec3 scale{1.0f};

    /// Rotation converted to Euler angles in degrees.
    /// @note Lossy round-trip: many Euler triples map to the same rotation, so
    ///       the values may not match what was passed to `set_euler_degrees()`.
    ///       Keep the quaternion as the source of truth when accumulating.
    [[nodiscard]] glm::vec3 euler_degrees() const {
        return glm::degrees(glm::eulerAngles(rotation));
    }

    /// Set rotation from Euler angles in degrees (pitch X, yaw Y, roll Z).
    /// Replaces the rotation outright rather than composing with it.
    void set_euler_degrees(const glm::vec3& degrees) {
        rotation = glm::quat(glm::radians(degrees));
    }

    /// Build the local TRS matrix (translate * rotate * scale).
    /// @note Local only — it does not include parent transforms, so it is not
    ///       the node's world matrix.
    [[nodiscard]] glm::mat4 matrix() const {
        auto m = glm::translate(glm::mat4(1.0f), position);
        m *= glm::mat4_cast(rotation);
        m = glm::scale(m, scale);
        return m;
    }
};

/// Decompose a TRS matrix back into position, rotation, and scale.
/// @param m A matrix built from translation, rotation, and scale only.
/// @return The extracted components.
/// @note Assumes no shear or negative scale; skewed matrices decompose to
///       nonsense rather than being rejected.
[[nodiscard]] inline Transform decompose_matrix(const glm::mat4& m) {
    Transform t;
    t.position = glm::vec3(m[3]);

    glm::vec3 c0{m[0][0], m[0][1], m[0][2]};
    glm::vec3 c1{m[1][0], m[1][1], m[1][2]};
    glm::vec3 c2{m[2][0], m[2][1], m[2][2]};

    t.scale.x = glm::length(c0);
    t.scale.y = glm::length(c1);
    t.scale.z = glm::length(c2);

    glm::mat3 rot(c0 / t.scale.x, c1 / t.scale.y, c2 / t.scale.z);
    t.rotation = glm::quat_cast(rot);

    return t;
}

} // namespace fjell

#endif // FJELL_TRANSFORM_HPP
