#include "renderer/resources/sdf_generator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

using namespace fjell;

namespace {

struct TriMesh {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    glm::vec3 aabb_min{1e30f};
    glm::vec3 aabb_max{-1e30f};

    // Adds a quad as two triangles with its own four vertices, the way an
    // imported mesh with flat normals arrives: nothing is shared across faces.
    void quad(glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        auto base = static_cast<uint32_t>(vertices.size());
        for (auto v : {a, b, c, d}) {
            vertices.push_back(v);
            aabb_min = glm::min(aabb_min, v);
            aabb_max = glm::max(aabb_max, v);
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
};

// Unit cube centred on the origin, counter-clockwise from outside.
TriMesh make_cube() {
    TriMesh m;
    const float h = 0.5f;
    m.quad({-h, -h,  h}, { h, -h,  h}, { h,  h,  h}, {-h,  h,  h}); // +z
    m.quad({ h, -h, -h}, {-h, -h, -h}, {-h,  h, -h}, { h,  h, -h}); // -z
    m.quad({-h,  h,  h}, { h,  h,  h}, { h,  h, -h}, {-h,  h, -h}); // +y
    m.quad({-h, -h, -h}, { h, -h, -h}, { h, -h,  h}, {-h, -h,  h}); // -y
    m.quad({ h, -h,  h}, { h, -h, -h}, { h,  h, -h}, { h,  h,  h}); // +x
    m.quad({-h, -h, -h}, {-h, -h,  h}, {-h,  h,  h}, {-h,  h, -h}); // -x
    return m;
}

// Single quad in the y=0 plane facing +y: an open surface, as ground planes are.
TriMesh make_plane() {
    TriMesh m;
    m.quad({-5.0f, 0.0f,  5.0f}, { 5.0f, 0.0f,  5.0f}, { 5.0f, 0.0f, -5.0f}, {-5.0f, 0.0f, -5.0f});
    return m;
}

// L-shaped prism: the footprint [0,2]x[0,1] union [0,1]x[1,2] in xy, extruded
// along z from 0 to 1. Closed, counter-clockwise from outside, with a concave
// edge running along (1, 1, z).
TriMesh make_l_prism() {
    TriMesh m;
    const glm::vec2 p[6] = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    auto at = [&](int i, float z) { return glm::vec3(p[i].x, p[i].y, z); };
    // Side walls, one quad per footprint edge, outward facing
    for (int i = 0; i < 6; ++i) {
        int j = (i + 1) % 6;
        m.quad(at(i, 0.0f), at(j, 0.0f), at(j, 1.0f), at(i, 1.0f));
    }
    // Caps as two rectangles each, split at the concave corner
    m.quad({0, 0, 1}, {2, 0, 1}, {2, 1, 1}, {0, 1, 1}); // +z lower rect
    m.quad({0, 1, 1}, {1, 1, 1}, {1, 2, 1}, {0, 2, 1}); // +z upper rect
    m.quad({0, 1, 0}, {2, 1, 0}, {2, 0, 0}, {0, 0, 0}); // -z lower rect
    m.quad({0, 2, 0}, {1, 2, 0}, {1, 1, 0}, {0, 1, 0}); // -z upper rect
    return m;
}

SdfResult bake(const TriMesh& m, uint32_t resolution = 48) {
    return generate_sdf(m.vertices.data(), static_cast<uint32_t>(m.vertices.size()),
                        m.indices.data(), static_cast<uint32_t>(m.indices.size()),
                        m.aabb_min, m.aabb_max, resolution);
}

// Value of the voxel containing a world point, in normalised int16 units.
float sample(const SdfResult& sdf, glm::vec3 p) {
    glm::vec3 uvw = (p - sdf.sdf_min) / (sdf.sdf_max - sdf.sdf_min);
    glm::ivec3 v = glm::clamp(glm::ivec3(uvw * float(sdf.resolution)),
                              glm::ivec3(0), glm::ivec3(int(sdf.resolution) - 1));
    auto r = sdf.resolution;
    return float(sdf.data[v.z * r * r + v.y * r + v.x]);
}

} // namespace

TEST_CASE("SDF of a closed cube is negative inside and positive outside", "[sdf]") {
    auto sdf = bake(make_cube());
    CHECK(sample(sdf, {0.0f, 0.0f, 0.0f}) < 0.0f);
    CHECK(sample(sdf, {0.3f, -0.3f, 0.2f}) < 0.0f);
    CHECK(sample(sdf, {0.0f, 0.7f, 0.0f}) > 0.0f);
    CHECK(sample(sdf, {0.7f, 0.7f, 0.7f}) > 0.0f);  // outside, closest feature is a corner
    CHECK(sample(sdf, {0.7f, 0.7f, 0.0f}) > 0.0f);  // outside, closest feature is an edge
}

TEST_CASE("SDF distance magnitude matches the surface distance", "[sdf]") {
    auto sdf = bake(make_cube());
    float max_dist = glm::length(sdf.sdf_max - sdf.sdf_min);
    float voxel = (sdf.sdf_max.x - sdf.sdf_min.x) / float(sdf.resolution);
    // The centre is 0.5 from every face, give or take a voxel
    float inside = sample(sdf, {0.0f, 0.0f, 0.0f}) / 32767.0f * max_dist;
    CHECK(inside > -0.5f - voxel);
    CHECK(inside < -0.5f + voxel);
    // Just above the top face, still inside the baked grid's 10% margin
    float outside = sample(sdf, {0.0f, 0.57f, 0.0f}) / 32767.0f * max_dist;
    CHECK(outside > 0.07f - voxel);
    CHECK(outside < 0.07f + voxel);
}

TEST_CASE("SDF of an open plane behaves as a half-space", "[sdf]") {
    auto sdf = bake(make_plane());
    CHECK(sample(sdf, {0.0f, 0.3f, 0.0f}) > 0.0f);
    CHECK(sample(sdf, {0.0f, -0.3f, 0.0f}) < 0.0f);
    CHECK(sample(sdf, {4.0f, -0.3f, -4.0f}) < 0.0f);
}

TEST_CASE("SDF sign is right next to a concave edge", "[sdf]") {
    auto sdf = bake(make_l_prism());
    // Both points are nearest to the concave edge at (1, 1, z); the two faces
    // meeting there disagree about which side is inside, so a single face
    // normal cannot classify them.
    CHECK(sample(sdf, {0.8f, 0.8f, 0.5f}) < 0.0f);
    CHECK(sample(sdf, {1.2f, 1.2f, 0.5f}) > 0.0f);
    // Sanity on the rest of the shape
    CHECK(sample(sdf, {1.5f, 0.5f, 0.5f}) < 0.0f);
    CHECK(sample(sdf, {0.5f, 1.5f, 0.5f}) < 0.0f);
    CHECK(sample(sdf, {1.5f, 1.5f, 0.5f}) > 0.0f);
    CHECK(sample(sdf, {0.5f, 0.5f, 1.3f}) > 0.0f);
}
