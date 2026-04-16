// debug_api.hpp — Static debug draw API for game scripts
#pragma once

#include "fjell/api.h"

#include <glm/glm.hpp>

extern "C" {
FJELL_SDK_EXPORT void fjell_debug_line(float x0, float y0, float z0,
                                       float x1, float y1, float z1,
                                       float r, float g, float b, float a,
                                       float ttl);
FJELL_SDK_EXPORT void fjell_debug_box(float cx, float cy, float cz,
                                      float hx, float hy, float hz,
                                      float r, float g, float b, float a,
                                      float ttl);
FJELL_SDK_EXPORT void fjell_debug_sphere(float cx, float cy, float cz,
                                         float radius,
                                         float r, float g, float b, float a,
                                         float ttl);
FJELL_SDK_EXPORT void fjell_debug_point(float x, float y, float z,
                                        float r, float g, float b, float a,
                                        float ttl);
}

namespace fjell {

struct Debug {
    /// Draw a line from `from` to `to`. Duration 0 = single frame.
    static void line(const glm::vec3& from, const glm::vec3& to,
                     const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                     float duration = 0.0f) {
        fjell_debug_line(from.x, from.y, from.z, to.x, to.y, to.z,
                         color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a wireframe box centered at `center` with `half_extents`.
    static void box(const glm::vec3& center, const glm::vec3& half_extents,
                    const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                    float duration = 0.0f) {
        fjell_debug_box(center.x, center.y, center.z,
                        half_extents.x, half_extents.y, half_extents.z,
                        color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a wireframe sphere at `center` with `radius`.
    static void sphere(const glm::vec3& center, float radius,
                       const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                       float duration = 0.0f) {
        fjell_debug_sphere(center.x, center.y, center.z, radius,
                           color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a point (rendered as a small cross) at `pos`.
    static void point(const glm::vec3& pos,
                      const glm::vec4& color = {1.0f, 1.0f, 0.0f, 1.0f},
                      float duration = 0.0f) {
        fjell_debug_point(pos.x, pos.y, pos.z,
                          color.r, color.g, color.b, color.a, duration);
    }

    /// Draw a ray from `origin` in `direction` (length = magnitude of direction).
    static void ray(const glm::vec3& origin, const glm::vec3& direction,
                    const glm::vec4& color = {0.0f, 1.0f, 0.0f, 1.0f},
                    float duration = 0.0f) {
        line(origin, origin + direction, color, duration);
    }
};

} // namespace fjell
