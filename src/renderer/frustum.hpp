#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace fjell {

struct AABB {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    void expand(const glm::vec3& point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }

    [[nodiscard]] bool valid() const {
        return min.x <= max.x;
    }

    [[nodiscard]] bool intersects(const AABB& other) const {
        return max.x >= other.min.x && min.x <= other.max.x &&
               max.y >= other.min.y && min.y <= other.max.y &&
               max.z >= other.min.z && min.z <= other.max.z;
    }

    // Arvo's fast AABB transform — no 8-corner expansion needed
    [[nodiscard]] AABB transform(const glm::mat4& m) const {
        glm::vec3 new_min = glm::vec3(m[3]);
        glm::vec3 new_max = new_min;

        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                float a = m[j][i] * min[j];
                float b = m[j][i] * max[j];
                new_min[i] += std::min(a, b);
                new_max[i] += std::max(a, b);
            }
        }

        return {new_min, new_max};
    }
};

struct Frustum {
    std::array<glm::vec4, 6> planes;

    // Extract from VP matrix with [-1,1] depth range (glm::perspective)
    [[nodiscard]] static Frustum from_vp_matrix(const glm::mat4& vp) {
        Frustum f;
        auto row = [&](int i) {
            return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]);
        };

        f.planes[0] = row(3) + row(0); // left
        f.planes[1] = row(3) - row(0); // right
        f.planes[2] = row(3) + row(1); // bottom
        f.planes[3] = row(3) - row(1); // top
        f.planes[4] = row(3) + row(2); // near
        f.planes[5] = row(3) - row(2); // far

        for (auto& p : f.planes) {
            float len = glm::length(glm::vec3(p));
            p /= len;
        }

        return f;
    }

    // Extract from VP matrix with [0,1] depth range (glm::orthoZO / perspectiveZO)
    [[nodiscard]] static Frustum from_vp_matrix_zo(const glm::mat4& vp) {
        Frustum f;
        auto row = [&](int i) {
            return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]);
        };

        f.planes[0] = row(3) + row(0); // left
        f.planes[1] = row(3) - row(0); // right
        f.planes[2] = row(3) + row(1); // bottom
        f.planes[3] = row(3) - row(1); // top
        f.planes[4] = row(2);          // near (ZO: just row2)
        f.planes[5] = row(3) - row(2); // far

        for (auto& p : f.planes) {
            float len = glm::length(glm::vec3(p));
            p /= len;
        }

        return f;
    }

    // Create a shadow caster frustum: same as the light frustum but without
    // the near plane, so objects behind the cascade (toward the light) are
    // included as potential shadow casters.
    [[nodiscard]] Frustum without_near_plane() const {
        Frustum f = *this;
        // Push near plane to -infinity (always passes)
        f.planes[4] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        return f;
    }

    // Conservative AABB intersection — positive vertex test
    [[nodiscard]] bool intersects(const AABB& aabb) const {
        for (const auto& plane : planes) {
            glm::vec3 p;
            p.x = (plane.x >= 0.0f) ? aabb.max.x : aabb.min.x;
            p.y = (plane.y >= 0.0f) ? aabb.max.y : aabb.min.y;
            p.z = (plane.z >= 0.0f) ? aabb.max.z : aabb.min.z;

            if (glm::dot(glm::vec3(plane), p) + plane.w < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

} // namespace fjell
