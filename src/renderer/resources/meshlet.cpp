#include "renderer/resources/meshlet.hpp"
#include "renderer/resources/vertex.hpp"
#include "core/log.hpp"

#include <meshoptimizer.h>

namespace fjell {

MeshletBuildResult build_meshlets(const Vertex* vertices, uint32_t vertex_count,
                                  const uint32_t* indices, uint32_t index_count) {
    MeshletBuildResult result;
    if (index_count == 0) return result;

    size_t max_meshlets = meshopt_buildMeshletsBound(index_count, MAX_MESHLET_VERTICES, MAX_MESHLET_TRIANGLES);

    std::vector<meshopt_Meshlet> mo_meshlets(max_meshlets);
    std::vector<uint32_t> mo_vertices(max_meshlets * MAX_MESHLET_VERTICES);
    std::vector<uint8_t> mo_triangles(max_meshlets * MAX_MESHLET_TRIANGLES * 3);

    size_t meshlet_count = meshopt_buildMeshlets(
        mo_meshlets.data(), mo_vertices.data(), mo_triangles.data(),
        indices, index_count,
        &vertices[0].position.x, vertex_count, sizeof(Vertex),
        MAX_MESHLET_VERTICES, MAX_MESHLET_TRIANGLES,
        0.5f); // cone_weight: balance cluster size vs cone culling efficiency

    mo_meshlets.resize(meshlet_count);

    result.meshlets.reserve(meshlet_count);
    result.bounds.reserve(meshlet_count);

    for (size_t i = 0; i < meshlet_count; ++i) {
        const auto& mo = mo_meshlets[i];

        // Compute bounds using meshoptimizer
        auto mb = meshopt_computeMeshletBounds(
            &mo_vertices[mo.vertex_offset],
            &mo_triangles[mo.triangle_offset],
            mo.triangle_count,
            &vertices[0].position.x, vertex_count, sizeof(Vertex));

        // Copy vertex indices
        uint32_t vert_base = static_cast<uint32_t>(result.meshlet_vertices.size());
        for (uint32_t v = 0; v < mo.vertex_count; ++v) {
            result.meshlet_vertices.push_back(mo_vertices[mo.vertex_offset + v]);
        }

        // Copy triangle indices (already 3 bytes per triangle)
        uint32_t tri_base = static_cast<uint32_t>(result.meshlet_triangles.size());
        for (uint32_t t = 0; t < mo.triangle_count * 3; ++t) {
            result.meshlet_triangles.push_back(mo_triangles[mo.triangle_offset + t]);
        }

        Meshlet m{};
        m.vertex_offset = vert_base;
        m.triangle_offset = tri_base / 3; // shader expects offset in triangles
        m.vertex_count = mo.vertex_count;
        m.triangle_count = mo.triangle_count;

        MeshletBounds bounds{};
        bounds.bounding_sphere = glm::vec4(mb.center[0], mb.center[1], mb.center[2], mb.radius);
        bounds.cone_apex = glm::vec4(mb.cone_apex[0], mb.cone_apex[1], mb.cone_apex[2], 0.0f);
        bounds.cone_axis_cutoff = glm::vec4(mb.cone_axis[0], mb.cone_axis[1], mb.cone_axis[2], mb.cone_cutoff);

        result.meshlets.push_back(m);
        result.bounds.push_back(bounds);
    }

    FJELL_GFX_DEBUG("Built {} meshlets from {} triangles",
                    meshlet_count, index_count / 3);

    return result;
}

} // namespace fjell
