#include "renderer/resources/sdf_generator.hpp"

#include <algorithm>
#include <cmath>

namespace fjell {

static float point_triangle_distance(const glm::vec3& p,
                                      const glm::vec3& a,
                                      const glm::vec3& b,
                                      const glm::vec3& c) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return glm::length(p - a);

    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return glm::length(p - b);

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return glm::length(p - (a + ab * v));
    }

    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return glm::length(p - c);

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return glm::length(p - (a + ac * w));
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return glm::length(p - (b + (c - b) * w));
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return glm::length(p - (a + ab * v + ac * w));
}

static bool ray_triangle_intersect(const glm::vec3& origin, const glm::vec3& dir,
                                    const glm::vec3& a, const glm::vec3& b,
                                    const glm::vec3& c) {
    glm::vec3 e1 = b - a, e2 = c - a;
    glm::vec3 h = glm::cross(dir, e2);
    float det = glm::dot(e1, h);
    if (std::abs(det) < 1e-8f) return false;
    float inv = 1.0f / det;
    glm::vec3 s = origin - a;
    float u = inv * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;
    glm::vec3 q = glm::cross(s, e1);
    float v = inv * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = inv * glm::dot(e2, q);
    return t > 1e-8f;
}

SdfResult generate_sdf(const glm::vec3* vertices, uint32_t vertex_count,
                       const uint32_t* indices, uint32_t index_count,
                       const glm::vec3& aabb_min, const glm::vec3& aabb_max,
                       uint32_t resolution, float margin_fraction) {
    SdfResult result;
    if (vertex_count == 0 || index_count == 0) return result;

    glm::vec3 extent = aabb_max - aabb_min;
    glm::vec3 margin = extent * margin_fraction;
    result.sdf_min = aabb_min - margin;
    result.sdf_max = aabb_max + margin;
    result.resolution = resolution;
    glm::vec3 sdf_extent = result.sdf_max - result.sdf_min;
    float max_dist = glm::length(sdf_extent);

    uint32_t res = resolution;
    uint32_t tri_count = index_count / 3;
    result.data.resize(res * res * res);

    for (uint32_t z = 0; z < res; ++z) {
        for (uint32_t y = 0; y < res; ++y) {
            for (uint32_t x = 0; x < res; ++x) {
                glm::vec3 p = result.sdf_min + sdf_extent * glm::vec3(
                    (float(x) + 0.5f) / float(res),
                    (float(y) + 0.5f) / float(res),
                    (float(z) + 0.5f) / float(res));

                float min_d = max_dist;
                for (uint32_t t = 0; t < tri_count; ++t) {
                    float d = point_triangle_distance(p,
                        vertices[indices[t * 3]],
                        vertices[indices[t * 3 + 1]],
                        vertices[indices[t * 3 + 2]]);
                    if (d < min_d) min_d = d;
                }

                uint32_t hits = 0;
                glm::vec3 ray_dir(1.0f, 0.0f, 0.0f);
                for (uint32_t t = 0; t < tri_count; ++t) {
                    if (ray_triangle_intersect(p, ray_dir,
                        vertices[indices[t * 3]],
                        vertices[indices[t * 3 + 1]],
                        vertices[indices[t * 3 + 2]])) {
                        ++hits;
                    }
                }
                if (hits & 1) min_d = -min_d;

                float normalized = std::clamp(min_d / max_dist, -1.0f, 1.0f);
                result.data[z * res * res + y * res + x] =
                    static_cast<int16_t>(normalized * 32767.0f);
            }
        }
    }

    return result;
}

} // namespace fjell
