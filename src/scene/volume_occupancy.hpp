#pragma once

#include "physics/physics_types.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace fjell {

/// How much of a point sits inside a placed volume, 0 outside and 1 well
/// within, falling off smoothly across the volume's edge: the CPU half of
/// `volume_occupancy()` in shaders/include/fjell_volume_shape.glsl, which
/// tests/test_volume_occupancy.cpp compiles as C++ and holds this to. Change
/// one and the other together.
///
/// The shape is the unit box or unit sphere in the volume's own space, so
/// `inv_transform` (the inverse of the node's world matrix) carries position,
/// rotation and extent. `edge_falloff` is the fraction of the extent the edge
/// fades across. Capsule and Mesh read as the box, as on the GPU.
[[nodiscard]] inline float volume_occupancy(ColliderShape shape, const glm::mat4& inv_transform,
                                            const glm::vec3& world_pos, float edge_falloff) {
    const glm::vec3 local{inv_transform * glm::vec4(world_pos, 1.0f)};
    const float fade = std::clamp(edge_falloff, 0.0001f, 1.0f);
    const auto smoothstep = [](float e0, float e1, float x) {
        const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    if (shape == ColliderShape::Sphere) {
        return 1.0f - smoothstep(1.0f - fade, 1.0f, glm::length(local));
    }
    // The box fades on whichever axis is nearest its face, so a corner fades
    // once rather than three times over.
    const glm::vec3 d = glm::abs(local);
    return 1.0f - smoothstep(1.0f - fade, 1.0f, std::max(d.x, std::max(d.y, d.z)));
}

} // namespace fjell
