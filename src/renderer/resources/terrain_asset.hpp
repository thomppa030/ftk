#pragma once

#include "core/result.hpp"
#include "renderer/resources/fjterrain.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/vertex.hpp"

#include <glm/glm.hpp>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <vector>

namespace fjell {

class Image;
class UploadContext;

/// A loaded .fjterrain: the CPU height data plus the GPU texture the mesh
/// shader samples to place vertices.
///
/// Heights deliberately live in two places and no more. The texture is
/// write-only from the CPU's side — geometry is generated from it every frame
/// and it is never read back — while the CPU array backs physics, queries and
/// editing. A third copy would be a third thing to keep in sync, and a height
/// array that disagrees with collision puts characters through visibly solid
/// ground.
class TerrainAsset {
public:
    TerrainAsset() = default;
    ~TerrainAsset();

    TerrainAsset(const TerrainAsset&) = delete;
    TerrainAsset& operator=(const TerrainAsset&) = delete;
    TerrainAsset(TerrainAsset&&) = delete;
    TerrainAsset& operator=(TerrainAsset&&) = delete;

    /// Load a .fjterrain and upload its heights as a single-channel texture.
    [[nodiscard]] static Result<std::unique_ptr<TerrainAsset>> load(
        const std::string& path, VkDevice device, VmaAllocator allocator,
        VkPhysicalDevice physical_device, UploadContext& upload);

    [[nodiscard]] const FjTerrainData& data() const { return data_; }
    [[nodiscard]] const std::string& path() const { return path_; }

    /// The height texture is not a `Texture`: that class hardcodes 4 bytes per
    /// pixel when sizing its staging copy, and heights are single-channel
    /// 16-bit. Image + sampler are owned here instead.
    [[nodiscard]] VkImageView height_view() const;
    [[nodiscard]] VkSampler height_sampler() const { return sampler_; }

    [[nodiscard]] uint32_t sample_count() const { return data_.sample_count(); }
    [[nodiscard]] uint32_t patch_quads() const { return data_.patch_quads(); }
    [[nodiscard]] uint32_t patches_per_side() const { return data_.patches_per_side(); }
    [[nodiscard]] float world_size() const { return data_.header.world_size; }
    [[nodiscard]] float height_scale() const { return data_.header.height_scale; }
    [[nodiscard]] float height_offset() const { return data_.header.height_offset; }

    // ── Placement ──
    // The node's pivot is the centre of the field, so a terrain parented to
    // an empty sits around it like any other child. Field coordinates —
    // what height_at() and the brush use — are metres from the min corner,
    // which is this far from the pivot:
    [[nodiscard]] glm::vec3 local_min_corner() const {
        const float half = data_.header.world_size * 0.5F;
        return {-half, 0.0F, -half};
    }
    /// The field's min corner in world space, from the node's world matrix.
    /// Only the translation is honoured here; rotation and scale are applied
    /// by physics but not yet by the renderer.
    [[nodiscard]] glm::vec3 world_min_corner(const glm::mat4& world_matrix) const {
        return glm::vec3{world_matrix[3]} + local_min_corner();
    }

    /// World-space height range across the whole field, measured at load. The
    /// nominal range (offset..offset+scale) is what the format *allows*; this is
    /// what the data actually uses, which keeps a bounding box from claiming
    /// empty air above a flat terrain.
    [[nodiscard]] float min_world_height() const;
    [[nodiscard]] float max_world_height() const;

    /// Terrain-local triangle mesh for ray tracing, decimated from the height
    /// array. Rendering needs no such thing — the mesh shader derives vertices
    /// from the texture — but an acceleration structure has to be built from
    /// real triangles, and GI rays do not resolve the full sample density.
    ///
    /// Positions are in the node's local space — centred on the pivot, see
    /// local_min_corner() — so the TLAS instance transform is the node's
    /// world matrix.
    ///
    /// Built once at load. Editing the heights does not rebuild it: the
    /// mega-buffer only appends, so re-uploading per stroke would leak, and a
    /// BLAS rebuild per stroke is not cheap. Until a rebuild lands, ray
    /// tracing sees the terrain as it was loaded.
    [[nodiscard]] const std::vector<Vertex>& ray_vertices() const { return ray_vertices_; }
    [[nodiscard]] const std::vector<uint32_t>& ray_indices() const { return ray_indices_; }

    /// Where the ray mesh lives in the mega-buffer, once uploaded. Empty until
    /// something calls upload_ray_mesh().
    [[nodiscard]] const MegaMeshEntry& ray_mesh_entry() const { return ray_mesh_entry_; }
    [[nodiscard]] bool ray_mesh_uploaded() const { return ray_mesh_entry_.index_count > 0; }

    /// Upload the ray mesh into the mega-buffer. Idempotent.
    void upload_ray_mesh(MegaBuffer& mega);

    /// Copy a rectangle of normalized heights out, row-major, `size_x * size_z`
    /// floats. The rect is clipped to the field; clipped-away cells are left
    /// untouched in `out`. This is what an undo entry captures before a stroke.
    void copy_heights(uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z,
                      float* out) const;

    /// Overwrite a rectangle of normalized heights and push that region to the
    /// GPU. `heights` is row-major, `size_x * size_z` floats, clamped to [0,1].
    /// The rect is clipped to the field.
    ///
    /// Only the touched region is uploaded, so a brush stroke costs the render
    /// side one small copy — geometry is regenerated from the texture every
    /// frame and needs no rebuild. The change is visible on the next frame
    /// that samples the texture. Physics is not updated here: the editor has
    /// no physics world, and a play-time body is the caller's to refresh
    /// through PhysicsWorld::update_terrain_heights.
    void apply_heights(uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z,
                       const float* heights, UploadContext& upload);

    /// True when apply_heights has changed the data since load or the last
    /// successful save.
    [[nodiscard]] bool dirty() const { return dirty_; }

    /// Write the current heights back to the .fjterrain this was loaded from.
    [[nodiscard]] Result<> save();

    /// Normalized height at a sample index, clamped at the edges.
    [[nodiscard]] float height_at_sample(uint32_t x, uint32_t z) const;

    /// World-space height (before the node's Y) at a point in field metres
    /// from the min corner, bilinearly filtered to match what the shader
    /// reconstructs between samples. Outside the terrain this clamps to the
    /// edge.
    [[nodiscard]] float height_at(float local_x, float local_z) const;

private:
    /// Decimate the height array into the triangle mesh ray tracing needs.
    void build_ray_mesh();

    FjTerrainData data_;
    std::string path_;
    std::vector<Vertex> ray_vertices_;
    std::vector<uint32_t> ray_indices_;
    MegaMeshEntry ray_mesh_entry_{};
    /// Recompute the world height range from the data when an edit has
    /// invalidated it. Lazy so a stroke of many small edits scans the field
    /// once, when a reader next asks, rather than once per brush dab.
    void ensure_height_range() const;

    mutable float min_world_height_{0.0F};
    mutable float max_world_height_{0.0F};
    mutable bool height_range_stale_{false};
    bool dirty_{false};
    VkDevice device_{VK_NULL_HANDLE};
    std::unique_ptr<Image> height_image_;
    VkSampler sampler_{VK_NULL_HANDLE};
};

} // namespace fjell
