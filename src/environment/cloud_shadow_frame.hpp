#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace fjell {

/// The square of ground the cloud shadow map covers this frame, and the
/// matrix that maps a world position onto it.
struct CloudShadowFrame {
    glm::vec2 min_xz{0.0f};      // world x,z of the map's corner
    float size{256.0f};          // metres across
    /// world position → (u, v, 0, 1); u and v in 0..1 inside the square.
    glm::mat4 world_to_uv{1.0f};
};

/// A square of `size` metres centred on the camera, snapped to the map's
/// texel grid so the shadows stand still on the ground while the camera
/// moves: the map only ever shifts by whole texels.
[[nodiscard]] inline CloudShadowFrame cloud_shadow_frame(const glm::vec3& camera_pos, float size,
                                                         uint32_t resolution) {
    CloudShadowFrame frame;
    frame.size = size;
    const float texel = size / static_cast<float>(resolution > 0 ? resolution : 1);
    const float half = size * 0.5f;
    frame.min_xz = glm::vec2(std::floor((camera_pos.x - half) / texel) * texel,
                             std::floor((camera_pos.z - half) / texel) * texel);
    glm::mat4 m(0.0f);
    m[0][0] = 1.0f / size;                 // u from x
    m[2][1] = 1.0f / size;                 // v from z
    m[3][0] = -frame.min_xz.x / size;
    m[3][1] = -frame.min_xz.y / size;
    m[3][3] = 1.0f;
    frame.world_to_uv = m;
    return frame;
}

} // namespace fjell
