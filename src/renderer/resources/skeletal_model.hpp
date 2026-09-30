#pragma once

#include "animation/skeleton.hpp"
#include "renderer/frustum.hpp"
#include "renderer/resources/meshlet.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/resources/vertex.hpp"

#include <glm/glm.hpp>
#include "gpu/buffer.hpp"
#include "gpu/owned.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

// CPU-side data parsed from a .fjskel or skinned .fjmesh file.
struct FjSkinnedMeshData {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<glm::vec3> collision_vertices;
    std::vector<uint32_t> collision_indices;
    uint32_t joint_count{0};
    AABB aabb;
    // Skeleton data (populated when loading .fjskel)
    std::vector<FjSkelJoint> joints;
    std::vector<BoneCollider> colliders;
};

// The bone influences of one render vertex, kept on the CPU so the mesh can
// be posed without the GPU.
struct SkinInfluence {
    glm::uvec4 bone_ids;
    glm::vec4 bone_weights;
};

// A skinned mesh with per-model VBO/IBO (not MegaBuffer).
// Used for skeletal animation — different vertex stride (SkinnedVertex, 92 bytes)
// and separate draw path from static Model.
// When loaded from .fjskel, also owns the embedded Skeleton.
class MeshletBuffer;

namespace gpu {
class Device;
class RenderEncoder;
}

class SkeletalModel {
public:
    /// Uploads the vertices and indices through `gpu` and adds the meshlets
    /// to `meshlets`. Throws when the buffers cannot be made.
    SkeletalModel(gpu::Device& gpu, FjSkinnedMeshData&& data, MeshletBuffer& meshlets);

    // Parse a .fjskel file (mesh + embedded skeleton).
    [[nodiscard]] static FjSkinnedMeshData load_fjskel(const std::string& path);

    /// Sets the model's own vertices and 32-bit indices.
    void bind(gpu::RenderEncoder& pass) const;

    [[nodiscard]] uint32_t index_count() const { return index_count_; }
    [[nodiscard]] uint32_t joint_count() const { return joint_count_; }
    [[nodiscard]] const AABB& local_aabb() const { return local_aabb_; }
    [[nodiscard]] float bounding_radius() const { return bounding_radius_; }

    // CPU-side meshlet build for the GPU-driven skinned mesh-shader path.
    // One MeshletBuildResult covers the whole mesh (single LOD for now;
    // skinned LOD chains would need a per-LOD bone-weight remap that
    // hasn't been built yet).
    [[nodiscard]] const MeshletBuildResult& meshlets() const { return meshlets_; }

    // GPU range inside the shared MeshletBuffer. vertex_offset is unused on
    // the skinned path — the mesh shader fetches from vertex_buffer_ at zero
    // base.
    [[nodiscard]] const MeshletRange& meshlet_range() const { return meshlet_range_; }

    // Embedded skeleton (non-null when loaded from .fjskel).
    [[nodiscard]] const Skeleton* skeleton() const { return skeleton_.get(); }
    [[nodiscard]] const std::shared_ptr<Skeleton>& skeleton_shared() const { return skeleton_; }

    [[nodiscard]] const std::vector<glm::vec3>& collision_vertices() const {
        return collision_vertices_;
    }
    [[nodiscard]] const std::vector<uint32_t>& collision_indices() const {
        return collision_indices_;
    }

    // The rendered triangle list, indexing the positions pose_surface() writes.
    [[nodiscard]] const std::vector<uint32_t>& surface_indices() const {
        return surface_indices_;
    }

    // Model-space positions of every render vertex under `skin_matrices`, the
    // model * inverse-bind palette an animator produces (one entry per joint).
    // An empty palette yields the bind pose, which is also what the renderer
    // shows when no animator drives the mesh. `out` is resized to fit.
    void pose_surface(const std::vector<glm::mat4>& skin_matrices,
                      std::vector<glm::vec3>& out) const;

private:
    gpu::Owned<gpu::Buffer> vertex_buffer_;
    gpu::Owned<gpu::Buffer> index_buffer_;
    uint32_t index_count_{0};
    uint32_t joint_count_{0};
    AABB local_aabb_;
    float bounding_radius_{0.0f};
    std::vector<glm::vec3> collision_vertices_;
    std::vector<uint32_t> collision_indices_;
    std::vector<glm::vec3> surface_positions_;
    std::vector<SkinInfluence> surface_influences_;
    std::vector<uint32_t> surface_indices_;
    std::shared_ptr<Skeleton> skeleton_;
    MeshletBuildResult meshlets_;
    MeshletRange meshlet_range_;
};

} // namespace fjell
