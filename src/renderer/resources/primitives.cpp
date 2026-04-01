#include "renderer/resources/primitives.hpp"

#include <cmath>
#include <numbers>

namespace fjell::primitives {

PrimitiveMeshData cube() {
    const glm::vec3 white{1.0f, 1.0f, 1.0f};

    // 24 vertices (4 per face for proper UVs and flat normals)
    std::vector<Vertex> vertices = {
        // Front face (normal: 0,0,1 tangent: 1,0,0)
        {{-0.5f, -0.5f,  0.5f}, white, {0.0f, 1.0f}, { 0.0f,  0.0f,  1.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.5f, -0.5f,  0.5f}, white, {1.0f, 1.0f}, { 0.0f,  0.0f,  1.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.5f,  0.5f,  0.5f}, white, {1.0f, 0.0f}, { 0.0f,  0.0f,  1.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        {{-0.5f,  0.5f,  0.5f}, white, {0.0f, 0.0f}, { 0.0f,  0.0f,  1.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        // Back face (normal: 0,0,-1 tangent: -1,0,0)
        {{ 0.5f, -0.5f, -0.5f}, white, {0.0f, 1.0f}, { 0.0f,  0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, white, {1.0f, 1.0f}, { 0.0f,  0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, white, {1.0f, 0.0f}, { 0.0f,  0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, white, {0.0f, 0.0f}, { 0.0f,  0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}},
        // Top face (normal: 0,1,0 tangent: 1,0,0)
        {{-0.5f,  0.5f,  0.5f}, white, {0.0f, 1.0f}, { 0.0f,  1.0f,  0.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.5f,  0.5f,  0.5f}, white, {1.0f, 1.0f}, { 0.0f,  1.0f,  0.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, white, {1.0f, 0.0f}, { 0.0f,  1.0f,  0.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, white, {0.0f, 0.0f}, { 0.0f,  1.0f,  0.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        // Bottom face (normal: 0,-1,0 tangent: 1,0,0)
        {{-0.5f, -0.5f, -0.5f}, white, {0.0f, 1.0f}, { 0.0f, -1.0f,  0.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.5f, -0.5f, -0.5f}, white, {1.0f, 1.0f}, { 0.0f, -1.0f,  0.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 0.5f, -0.5f,  0.5f}, white, {1.0f, 0.0f}, { 0.0f, -1.0f,  0.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        {{-0.5f, -0.5f,  0.5f}, white, {0.0f, 0.0f}, { 0.0f, -1.0f,  0.0f}, { 1.0f, 0.0f, 0.0f, 1.0f}},
        // Right face (normal: 1,0,0 tangent: 0,0,-1)
        {{ 0.5f, -0.5f,  0.5f}, white, {0.0f, 1.0f}, { 1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f,-1.0f, 1.0f}},
        {{ 0.5f, -0.5f, -0.5f}, white, {1.0f, 1.0f}, { 1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f,-1.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, white, {1.0f, 0.0f}, { 1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f,-1.0f, 1.0f}},
        {{ 0.5f,  0.5f,  0.5f}, white, {0.0f, 0.0f}, { 1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f,-1.0f, 1.0f}},
        // Left face (normal: -1,0,0 tangent: 0,0,1)
        {{-0.5f, -0.5f, -0.5f}, white, {0.0f, 1.0f}, {-1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f, 1.0f, 1.0f}},
        {{-0.5f, -0.5f,  0.5f}, white, {1.0f, 1.0f}, {-1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f, 1.0f, 1.0f}},
        {{-0.5f,  0.5f,  0.5f}, white, {1.0f, 0.0f}, {-1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f, 1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, white, {0.0f, 0.0f}, {-1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f, 1.0f, 1.0f}},
    };

    std::vector<uint32_t> indices;
    indices.reserve(36);
    for (uint32_t face = 0; face < 6; ++face) {
        uint32_t base = face * 4;
        indices.push_back(base);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }

    return {std::move(vertices), std::move(indices)};
}

PrimitiveMeshData plane(uint32_t subdivisions) {
    const glm::vec3 white{1.0f, 1.0f, 1.0f};
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    const glm::vec4 tan{1.0f, 0.0f, 0.0f, 1.0f};

    uint32_t verts_per_side = subdivisions + 1;
    std::vector<Vertex> vertices;
    vertices.reserve(verts_per_side * verts_per_side);

    for (uint32_t z = 0; z < verts_per_side; ++z) {
        for (uint32_t x = 0; x < verts_per_side; ++x) {
            float u = static_cast<float>(x) / static_cast<float>(subdivisions);
            float v = static_cast<float>(z) / static_cast<float>(subdivisions);
            float px = u - 0.5f;
            float pz = 0.5f - v; // +Z is forward, UV.y=0 at front
            vertices.push_back({{px, 0.0f, pz}, white, {u, v}, up, tan});
        }
    }

    std::vector<uint32_t> indices;
    indices.reserve(subdivisions * subdivisions * 6);

    for (uint32_t z = 0; z < subdivisions; ++z) {
        for (uint32_t x = 0; x < subdivisions; ++x) {
            uint32_t tl = z * verts_per_side + x;
            uint32_t tr = tl + 1;
            uint32_t bl = (z + 1) * verts_per_side + x;
            uint32_t br = bl + 1;
            indices.push_back(tl);
            indices.push_back(tr);
            indices.push_back(br);
            indices.push_back(tl);
            indices.push_back(br);
            indices.push_back(bl);
        }
    }

    return {std::move(vertices), std::move(indices)};
}

PrimitiveMeshData sphere(uint32_t rings, uint32_t segments) {
    const glm::vec3 white{1.0f, 1.0f, 1.0f};
    constexpr float pi = std::numbers::pi_v<float>;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    for (uint32_t r = 0; r <= rings; ++r) {
        float phi = pi * static_cast<float>(r) / static_cast<float>(rings);
        float y = std::cos(phi) * 0.5f;
        float ring_radius = std::sin(phi) * 0.5f;

        for (uint32_t s = 0; s <= segments; ++s) {
            float theta = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            float x = ring_radius * std::cos(theta);
            float z = ring_radius * std::sin(theta);

            float u = static_cast<float>(s) / static_cast<float>(segments);
            float v = static_cast<float>(r) / static_cast<float>(rings);

            glm::vec3 pos{x, y, z};
            glm::vec3 normal = glm::normalize(pos);

            // Tangent: derivative of position with respect to theta
            glm::vec3 tan_dir{-std::sin(theta), 0.0f, std::cos(theta)};
            glm::vec4 tangent{glm::normalize(tan_dir), 1.0f};

            vertices.push_back({pos, white, {u, v}, normal, tangent});
        }
    }

    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < segments; ++s) {
            uint32_t a = r * (segments + 1) + s;
            uint32_t b = a + segments + 1;

            indices.push_back(a);
            indices.push_back(a + 1);
            indices.push_back(b);

            indices.push_back(a + 1);
            indices.push_back(b + 1);
            indices.push_back(b);
        }
    }

    return {std::move(vertices), std::move(indices)};
}

} // namespace fjell::primitives
