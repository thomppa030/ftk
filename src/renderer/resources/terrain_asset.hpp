#pragma once

#include "core/result.hpp"
#include "gpu/owned.hpp"
#include "gpu/texture.hpp"
#include "renderer/resources/fjterrain.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/vertex.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class Material;

namespace gpu {
class Device;
class Upload;
}

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
    [[nodiscard]] static Result<std::unique_ptr<TerrainAsset>> load(const std::string& path,
                                                                    gpu::Device& gpu);

    [[nodiscard]] const FjTerrainData& data() const { return data_; }
    [[nodiscard]] const std::string& path() const { return path_; }

    /// The heights, one normalized 16-bit sample per texel, which the mesh
    /// stage places vertices from.
    [[nodiscard]] gpu::Texture height_texture() const;

    /// Per-patch height bounds, one texel per patch, holding the lowest and
    /// highest normalized height that patch reaches. The task shader reads it
    /// to bound a patch in Y: without it a patch on flat ground claims the
    /// whole vertical extent of the terrain and culling keeps it.
    ///
    /// One texel per patch rather than a mip of the height texture, because
    /// the patch grid is the sample grid over `patch_quads` (11, prime) and no
    /// mip level lines up with it.
    [[nodiscard]] gpu::Texture patch_bounds_texture() const;

    /// Painted layer weights, one layer per channel, sampled by the fragment
    /// stage. The texture always covers the field at weight_count() texels a
    /// side; until the first paint stroke the CPU array stays empty and the
    /// texture reads as layer 0 everywhere.
    [[nodiscard]] gpu::Texture weight_texture() const;

    /// Weight texels per side. One per height sample unless the file was
    /// painted at another resolution.
    [[nodiscard]] uint32_t weight_count() const { return weight_side_; }
    [[nodiscard]] bool has_painted_weights() const { return !data_.weights.empty(); }

    /// Copy a rectangle of RGBA weights out, row-major, `size_x * size_z * 4`
    /// bytes. Clipped to the grid; an unpainted field reads as layer 0.
    void copy_weights(uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z,
                      uint8_t* out) const;

    /// Overwrite a rectangle of RGBA weights and push that region to the GPU.
    /// The first call on an unpainted field allocates the CPU array, which is
    /// what makes the weights part of the saved file. Same shape as
    /// apply_heights; there is nothing to tell physics.
    void apply_weights(uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z,
                       const uint8_t* rgba, gpu::Upload& upload);

    // ── Material layers ──
    // Up to FJTERRAIN_MAX_LAYERS ordinary .fjmat materials, blended by the
    // weight texture. The table lives in the .fjterrain; the resolved
    // Material pointers are runtime state the resource manager fills in.

    [[nodiscard]] uint32_t layer_count() const { return static_cast<uint32_t>(data_.layers.size()); }
    /// Project-relative .fjmat path, empty when the layer is unassigned.
    [[nodiscard]] const std::string& layer_material_path(uint32_t layer) const;
    /// Project-relative .fjsurface path, empty when the layer has no surface.
    [[nodiscard]] const std::string& layer_surface_path(uint32_t layer) const;
    /// Surface id physics reports where this layer dominates, 0 for none.
    [[nodiscard]] uint32_t layer_surface(uint32_t layer) const {
        return layer < layer_count() ? layer_surfaces_[layer] : 0;
    }
    [[nodiscard]] float layer_uv_scale(uint32_t layer) const;
    [[nodiscard]] Material* layer_material(uint32_t layer) const;
    /// GPU material row for a layer. A layer with no material, or a layer
    /// index beyond layer_count(), draws with the plain material the resource
    /// manager bound as the unassigned look, so every pushed row is valid.
    [[nodiscard]] uint32_t layer_material_id(uint32_t layer) const;

    /// Grow or shrink the layer table. New layers start unassigned with the
    /// default tiling. Marks the asset dirty.
    void set_layer_count(uint32_t count);
    void set_layer_material_path(uint32_t layer, const std::string& path);
    void set_layer_surface_path(uint32_t layer, const std::string& path);
    void set_layer_uv_scale(uint32_t layer, float metres_per_repeat);
    /// Runtime binding of a layer to its loaded material. Not persisted; the
    /// path is what the file holds.
    void set_layer_material(uint32_t layer, Material* material);
    /// What an unassigned layer draws with.
    void set_unassigned_layer_material(Material* material) { unassigned_material_ = material; }
    /// Runtime binding of a layer to its loaded surface id. Not persisted.
    void set_layer_surface(uint32_t layer, uint32_t surface_id);

    /// Which layer dominates the painted weights under each of the field's
    /// quads, as physics wants it: one byte per quad, row-major,
    /// `size_x * size_z` entries for the quad rect starting at (x, z). Quads
    /// outside the field are left untouched. Inline, like everything physics
    /// reads from the asset, so the physics world needs none of the GPU side.
    void dominant_layers(uint32_t x, uint32_t z, uint32_t size_x, uint32_t size_z,
                         uint8_t* out) const {
        const uint32_t samples = data_.sample_count();
        if (out == nullptr || samples < 2 || weight_side_ == 0) return;
        const uint32_t quads = samples - 1;
        const uint32_t side = weight_side_;
        const uint32_t x_end = std::min(x + size_x, quads);
        const uint32_t z_end = std::min(z + size_z, quads);
        for (uint32_t qz = z; qz < z_end; ++qz) {
            // The quad's centre, read from the weight grid the way the sampler
            // reads it: texel i covers [i, i + 1) / side of the field.
            const uint32_t tz = std::min(((2 * qz + 1) * side) / (2 * quads), side - 1);
            for (uint32_t qx = x; qx < x_end; ++qx) {
                const uint32_t tx = std::min(((2 * qx + 1) * side) / (2 * quads), side - 1);
                uint8_t best = 0;
                if (!data_.weights.empty()) {
                    const uint8_t* texel =
                        &data_.weights[((static_cast<size_t>(tz) * side) + tx) * 4];
                    for (uint8_t c = 1; c < 4; ++c) {
                        if (texel[c] > texel[best]) best = c;
                    }
                }
                out[(static_cast<size_t>(qz - z) * size_x) + (qx - x)] = best;
            }
        }
    }

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
    ///
    /// Translation only, everywhere: a terrain's size is its asset's
    /// `world_size` and its shape is a grid sampled along world X and Z, so
    /// the node places it and nothing more. Rotating or scaling a terrain
    /// node does nothing, by decision — the raster path, physics, ray tracing
    /// and the editor queries all read the transform this way, and any one of
    /// them reading it differently puts collision or rays where the ground is
    /// not drawn.
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

    /// Metres between two of the ray mesh's vertices. This is the scale over
    /// which the traced surface may depart from the rasterized one, so a ray
    /// spawned on the rasterized surface has to start at least this far out
    /// of it on sloping ground or it begins inside the geometry it is about
    /// to trace. Zero before the ray mesh is built.
    [[nodiscard]] float ray_mesh_spacing() const { return ray_mesh_spacing_; }
    [[nodiscard]] bool ray_mesh_uploaded() const { return ray_mesh_entry_.index_count > 0; }

    /// Upload the ray mesh into the mega-buffer. Idempotent.
    void upload_ray_mesh(MegaBuffer& mega);

    /// Rebuild the ray mesh from the current heights and push it back over the
    /// same mega-buffer region, if a sculpt has moved the ground since the
    /// last call. Cheap and idempotent when nothing changed, so a ray-tracing
    /// consumer can call it every frame it draws.
    ///
    /// The grid's topology never changes, only its heights, so the vertices
    /// are overwritten in place and the entry keeps its position. Returns true
    /// when the geometry changed, which is the signal to rebuild the BLAS
    /// describing it: the acceleration structure still points at valid memory
    /// but now describes triangles that have moved.
    bool refresh_ray_mesh(MegaBuffer& mega);

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
                       const float* heights, gpu::Upload& upload);

    /// A rectangle of height samples: what an edit touched.
    struct SampleRect {
        uint32_t x{0};
        uint32_t z{0};
        uint32_t width{0};
        uint32_t height{0};
        [[nodiscard]] bool empty() const { return width == 0 || height == 0; }
    };

    /// Counts every apply_heights() call, so a consumer holding a derived copy
    /// of the heights can tell the copy went stale without being told.
    [[nodiscard]] uint32_t edit_generation() const { return edit_generation_; }

    /// The samples edited since `since_generation`, for a consumer catching a
    /// derived copy up. Several consumers hold such copies — the scene SDF and
    /// the ray-tracing BLAS — and each is at its own generation, so each asks
    /// for its own rect rather than sharing one that the first reader clears.
    /// A consumer stores the returned `edit_generation()` and passes it back
    /// next time.
    ///
    /// Edits older than the ring are forgotten, so a consumer that has fallen
    /// further behind than EDIT_HISTORY strokes is told the whole field changed
    /// rather than being given a rect that would miss some of it. Returns an
    /// empty rect when nothing has changed.
    [[nodiscard]] SampleRect edits_since(uint32_t since_generation) const;

    /// True when the heights or the layer table changed since load or the last
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

    /// Bring the patch-bounds image up to date with height edits made since
    /// the last call. Cheap and idempotent when nothing changed: the terrain
    /// pass calls it every frame it draws. Kept off apply_heights() so a
    /// stroke of many dabs rebuilds each patch once per frame rather than once
    /// per dab.
    void refresh_patch_bounds(gpu::Upload& upload);

private:
    /// Decimate the height array into the triangle mesh ray tracing needs.
    void build_ray_mesh();

    /// Min and max normalized height over a rectangle of patches, written into
    /// `patch_bounds_texels_`.
    void compute_patch_bounds(uint32_t px, uint32_t pz, uint32_t count_x, uint32_t count_z);

    FjTerrainData data_;
    std::string path_;
    std::vector<Vertex> ray_vertices_;
    std::vector<uint32_t> ray_indices_;
    MegaMeshEntry ray_mesh_entry_{};
    float ray_mesh_spacing_{0.0F};
    /// Recompute the world height range from the data when an edit has
    /// invalidated it. Lazy so a stroke of many small edits scans the field
    /// once, when a reader next asks, rather than once per brush dab.
    void ensure_height_range() const;

    mutable float min_world_height_{0.0F};
    mutable float max_world_height_{0.0F};
    mutable bool height_range_stale_{false};
    bool dirty_{false};
    uint32_t edit_generation_{0};

    /// Recent edits, one entry per apply_heights() call, indexed by
    /// `generation % EDIT_HISTORY`. Deep enough that a consumer updating once
    /// a frame keeps up with a brush dragged at any plausible stroke rate, and
    /// small enough to stay in the asset rather than growing without bound.
    /// A consumer further behind than this is handed the whole field.
    static constexpr uint32_t EDIT_HISTORY = 64;
    std::array<SampleRect, EDIT_HISTORY> edit_history_{};
    gpu::Owned<gpu::Texture> height_texture_;
    gpu::Owned<gpu::Texture> weight_texture_;
    gpu::Owned<gpu::Texture> patch_bounds_texture_;
    /// Two unorm16 per patch, min then max, row-major over the patch grid.
    std::vector<uint16_t> patch_bounds_texels_;
    /// The edit generation `patch_bounds_texture_` is current as of.
    uint32_t patch_bounds_generation_{0};
    /// The edit generation the ray mesh was built from.
    uint32_t ray_mesh_generation_{0};
    uint32_t weight_side_{0};
    std::array<Material*, FJTERRAIN_MAX_LAYERS> layer_materials_{};
    std::array<uint32_t, FJTERRAIN_MAX_LAYERS> layer_surfaces_{};
    Material* unassigned_material_{nullptr};
};

} // namespace fjell
