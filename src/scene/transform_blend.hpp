#pragma once

#include "scene/transform.hpp"

#include <glm/gtc/quaternion.hpp>

namespace fjell {

/// The placement `t` of the way from `from` to `to`: position and scale
/// straight across, rotation along the shorter arc.
[[nodiscard]] inline Transform blend(const Transform& from, const Transform& to, float t) {
    return Transform{
        .position = glm::mix(from.position, to.position, t),
        .rotation = glm::slerp(from.rotation, to.rotation, t),
        .scale = glm::mix(from.scale, to.scale, t),
    };
}

} // namespace fjell
