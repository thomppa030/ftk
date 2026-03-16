#pragma once

#include "renderer/vertex.hpp"

#include <cstdint>
#include <vector>

namespace fjell {

struct PrimitiveMeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

namespace primitives {

[[nodiscard]] PrimitiveMeshData cube();
[[nodiscard]] PrimitiveMeshData plane();
[[nodiscard]] PrimitiveMeshData sphere(uint32_t rings = 16, uint32_t segments = 32);

} // namespace primitives
} // namespace fjell
