#include "renderer/primitives.hpp"

#include <cmath>
#include <numbers>

namespace fjell::primitives {

PrimitiveMeshData cube() {
    const glm::vec3 white{1.0f, 1.0f, 1.0f};

    // 24 vertices (4 per face for proper UVs)
    std::vector<Vertex> vertices = {
        // Front face
        {{-0.5f, -0.5f,  0.5f}, white, {0.0f, 1.0f}},
        {{ 0.5f, -0.5f,  0.5f}, white, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f,  0.5f}, white, {1.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, white, {0.0f, 0.0f}},
        // Back face
        {{ 0.5f, -0.5f, -0.5f}, white, {0.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, white, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, white, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, white, {0.0f, 0.0f}},
        // Top face
        {{-0.5f,  0.5f,  0.5f}, white, {0.0f, 1.0f}},
        {{ 0.5f,  0.5f,  0.5f}, white, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, white, {1.0f, 0.0f}},
        {{-0.5f,  0.5f, -0.5f}, white, {0.0f, 0.0f}},
        // Bottom face
        {{-0.5f, -0.5f, -0.5f}, white, {0.0f, 1.0f}},
        {{ 0.5f, -0.5f, -0.5f}, white, {1.0f, 1.0f}},
        {{ 0.5f, -0.5f,  0.5f}, white, {1.0f, 0.0f}},
        {{-0.5f, -0.5f,  0.5f}, white, {0.0f, 0.0f}},
        // Right face
        {{ 0.5f, -0.5f,  0.5f}, white, {0.0f, 1.0f}},
        {{ 0.5f, -0.5f, -0.5f}, white, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, white, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, white, {0.0f, 0.0f}},
        // Left face
        {{-0.5f, -0.5f, -0.5f}, white, {0.0f, 1.0f}},
        {{-0.5f, -0.5f,  0.5f}, white, {1.0f, 1.0f}},
        {{-0.5f,  0.5f,  0.5f}, white, {1.0f, 0.0f}},
        {{-0.5f,  0.5f, -0.5f}, white, {0.0f, 0.0f}},
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

PrimitiveMeshData plane() {
    const glm::vec3 white{1.0f, 1.0f, 1.0f};

    std::vector<Vertex> vertices = {
        {{-0.5f, 0.0f,  0.5f}, white, {0.0f, 0.0f}},
        {{ 0.5f, 0.0f,  0.5f}, white, {1.0f, 0.0f}},
        {{ 0.5f, 0.0f, -0.5f}, white, {1.0f, 1.0f}},
        {{-0.5f, 0.0f, -0.5f}, white, {0.0f, 1.0f}},
    };

    std::vector<uint32_t> indices = {0, 1, 2, 0, 2, 3};

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

            vertices.push_back({{x, y, z}, white, {u, v}});
        }
    }

    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < segments; ++s) {
            uint32_t a = r * (segments + 1) + s;
            uint32_t b = a + segments + 1;

            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(a + 1);

            indices.push_back(a + 1);
            indices.push_back(b);
            indices.push_back(b + 1);
        }
    }

    return {std::move(vertices), std::move(indices)};
}

} // namespace fjell::primitives
