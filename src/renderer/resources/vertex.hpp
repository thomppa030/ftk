#pragma once

#include "gpu/pipeline.hpp"

#include <glm/glm.hpp>

#include <cstddef>

namespace fjell {

struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec2 tex_coord;
    glm::vec3 normal;
    glm::vec4 tangent; // xyz = tangent direction, w = handedness (±1)

    /// The layout a pipeline reads these vertices with, as the GPU interface
    /// describes it.
    [[nodiscard]] static gpu::VertexLayout layout() {
        return {.stride = sizeof(Vertex),
                .attributes = {
                    {.location = 0, .format = gpu::VertexFormat::float3, .offset = offsetof(Vertex, position)},
                    {.location = 1, .format = gpu::VertexFormat::float3, .offset = offsetof(Vertex, color)},
                    {.location = 2, .format = gpu::VertexFormat::float2, .offset = offsetof(Vertex, tex_coord)},
                    {.location = 3, .format = gpu::VertexFormat::float3, .offset = offsetof(Vertex, normal)},
                    {.location = 4, .format = gpu::VertexFormat::float4, .offset = offsetof(Vertex, tangent)},
                }};
    }
};

struct SkinnedVertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec2 tex_coord;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::uvec4 bone_ids;     // up to 4 bone influences per vertex
    glm::vec4 bone_weights;  // corresponding weights (sum to 1.0)

    /// The layout a pipeline reads these vertices with, as the GPU interface
    /// describes it.
    [[nodiscard]] static gpu::VertexLayout layout() {
        return {.stride = sizeof(SkinnedVertex),
                .attributes = {
                    {.location = 0, .format = gpu::VertexFormat::float3, .offset = offsetof(SkinnedVertex, position)},
                    {.location = 1, .format = gpu::VertexFormat::float3, .offset = offsetof(SkinnedVertex, color)},
                    {.location = 2, .format = gpu::VertexFormat::float2, .offset = offsetof(SkinnedVertex, tex_coord)},
                    {.location = 3, .format = gpu::VertexFormat::float3, .offset = offsetof(SkinnedVertex, normal)},
                    {.location = 4, .format = gpu::VertexFormat::float4, .offset = offsetof(SkinnedVertex, tangent)},
                    {.location = 5, .format = gpu::VertexFormat::uint4, .offset = offsetof(SkinnedVertex, bone_ids)},
                    {.location = 6, .format = gpu::VertexFormat::float4, .offset = offsetof(SkinnedVertex, bone_weights)},
                }};
    }
};

} // namespace fjell
