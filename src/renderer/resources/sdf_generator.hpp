#pragma once

// SDF generation — shared between fjimport (offline) and engine (runtime primitives).
// No engine dependencies beyond GLM and standard library.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fjell {

struct SdfResult {
    std::vector<int16_t> data;
    uint32_t resolution{0};
    glm::vec3 sdf_min{0.0f};
    glm::vec3 sdf_max{0.0f};
};

// Generate a signed distance field grid from a triangle mesh.
// Resolution: grid cells per axis (e.g. 32).
// margin_fraction: how much extra space around the AABB (0.1 = 10% on each side).
[[nodiscard]] SdfResult generate_sdf(
    const glm::vec3* vertices, uint32_t vertex_count,
    const uint32_t* indices, uint32_t index_count,
    const glm::vec3& aabb_min, const glm::vec3& aabb_max,
    uint32_t resolution = 32, float margin_fraction = 0.1f);

} // namespace fjell
