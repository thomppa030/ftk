#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/frustum.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/uniform_types.hpp"

#include <gpu/buffer.hpp>
#include <gpu/format.hpp>
#include <gpu/owned.hpp>
#include <gpu/pipeline.hpp>
#include <gpu/sampler.hpp>
#include <gpu/texture.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace fjell {

class Model;
class TerrainAsset;
struct PassResources;

namespace gpu {
class CommandList;
}

// What an instance's grid holds: a baked mesh distance field, or a terrain's
// height samples that the shader turns into a distance on the fly.
enum class SdfInstanceKind : uint32_t { mesh = 0, terrain = 1 };

// Per-instance SDF metadata for the compositing shader (matches GLSL SdfInstance)
struct GpuSdfInstance {
    glm::mat4 world_to_sdf;    // inverse model matrix scaled to SDF grid space
    glm::vec3 sdf_min;         // local-space SDF volume min
    float sdf_scale;           // max_dist for denormalizing int16 values
    glm::vec3 sdf_max;         // local-space SDF volume max
    uint32_t grid_offset;      // offset into mesh SDF buffer (in int16 units)
    glm::uvec3 grid_resolution;
    uint32_t slot;             // the object's lasting id, written into the index volume
    glm::vec3 mesh_min;        // local-space mesh bounds, tighter than the grid
    float pad1;
    glm::vec3 mesh_max;
    float pad2;
    // Terrain instances only: the field's placement, so the shader can go
    // from a world position to a height sample without a matrix.
    SdfInstanceKind kind{SdfInstanceKind::mesh};
    uint32_t pad3;
    glm::vec2 terrain_origin;      // world XZ of the field's min corner
    float terrain_size;            // metres across
    float terrain_height_scale;    // world metres per unit of normalized height
    float terrain_height_offset;   // world Y of normalized height 0
    float pad4;
};
static_assert(sizeof(GpuSdfInstance) == 176);

/// The scene's signed distance field: one volume of distances clamped at
/// max_distance(), and beside it the index volume naming the object nearest
/// each voxel. Every frame the caller gathers the placed meshes and terrains
/// (begin_gather() .. end_gather()); the pass turns what moved, came or went
/// into regions and composes only those, first thing in the frame.
///
/// The volumes are exported as `scene_sdf` and `scene_sdf_index` and rest
/// where fragment and compute shaders sample them, which is how most readers
/// see them: through the global set. One compose a frame, in the first
/// viewport's graph that holds the pass; the others find nothing pending.
class SceneSdfPass : public RenderPass {
public:
    static constexpr uint32_t DEFAULT_RESOLUTION = 128;
    static constexpr gpu::Format SDF_FORMAT = gpu::Format::r16_float;
    /// The index volume: per voxel, the slot of the object nearest it within
    /// max_distance(), or NO_SDF_SLOT. A slot is an object's id for as long
    /// as it is in the scene, so a voxel left alone by later composes still
    /// names the right object.
    static constexpr gpu::Format SDF_INDEX_FORMAT = gpu::Format::r16_uint;
    /// What the volumes and the debug slice are exported under.
    static constexpr const char* VOLUME_NAME = "scene_sdf";
    static constexpr const char* INDEX_NAME = "scene_sdf_index";
    static constexpr const char* SLICE_NAME = "scene_sdf_slice";

    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void post_create(GpuContext& gpu, PassRegistry& passes) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    // Register a model's per-mesh SDF data. Returns true if data was uploaded.
    bool register_model_sdf(const Model& model, uintptr_t model_id);

    /// Where a terrain's height samples live in the packed grid buffer.
    struct TerrainSdfEntry {
        uint32_t offset;       // in int16 units
        uint32_t samples;      // per side
        uint32_t generation;   // TerrainAsset::edit_generation() the copy matches
    };

    /// Upload a terrain's heights into the grid buffer, once. A terrain has no
    /// baked distance field; the shader derives the distance from the heights
    /// it finds here. Null when the buffer is full.
    const TerrainSdfEntry* register_terrain_sdf(const TerrainAsset& terrain);

    /// Re-copy a rectangle of a registered terrain's heights after a sculpt
    /// edit, in place, and record the asset generation the copy now matches.
    void update_terrain_sdf(const TerrainAsset& terrain, uint32_t x, uint32_t z,
                            uint32_t size_x, uint32_t size_z);

    /// Mark a world-space box for recomputation without any instance having
    /// moved: what an edit inside a terrain needs.
    void invalidate_region(const AABB& world_bounds) { add_dirty_region(world_bounds); }

    /// Identifies one placed mesh across frames. Node ids repeat across
    /// scenes, so the scene id is part of the key.
    [[nodiscard]] static uint64_t instance_key(uint64_t scene_id, uint32_t object_id) {
        return (scene_id << 32) | object_id;
    }

    /// The field is rebuilt only where it can have changed. Each frame the
    /// caller hands over every placed mesh between begin_gather() and
    /// end_gather(); the pass remembers where each one was and turns moved,
    /// added and vanished instances into voxel regions it composes.
    void begin_gather();
    /// `world_bounds` is the mesh's bounding box in world space, `moved` says
    /// its transform changed since the previous frame, `materials` are the
    /// material ids it draws with (the first SDF_SLOT_MATERIALS count).
    void gather_instance(uint64_t key, const AABB& world_bounds, bool moved,
                         const GpuSdfInstance& instance, std::span<const uint32_t> materials);
    /// Ends the gather, and writes every object's materials into frame slot
    /// `frame_index`'s table.
    void end_gather(uint32_t frame_index);

    [[nodiscard]] bool has_pending_update() const {
        return full_rebuild_ || !dirty_regions_.empty();
    }

    // Set world-space bounds of the global SDF volume
    void set_bounds(const glm::vec3& min, const glm::vec3& max);

    /// Distances are clamped here: a voxel farther than this from every
    /// surface holds exactly this value. Sphere tracing stays conservative,
    /// and a moved mesh only touches voxels within this band of its bounds.
    [[nodiscard]] float max_distance() const;

    /// The distances, sampled trilinearly and clamped to the edge.
    [[nodiscard]] gpu::Texture volume() const { return volume_; }
    [[nodiscard]] gpu::Sampler sampler() const { return volume_sampler_; }
    /// The index volume (SDF_INDEX_FORMAT), read with texelFetch.
    [[nodiscard]] gpu::Texture index_volume() const { return index_volume_; }
    [[nodiscard]] gpu::Sampler index_sampler() const { return index_sampler_; }
    /// Frame slot `frame_index`'s GpuSdfSlot per slot the index volume names.
    [[nodiscard]] gpu::Buffer slot_table(uint32_t frame_index) const {
        return slot_tables_[frame_index];
    }
    [[nodiscard]] const glm::vec3& volume_min() const { return volume_min_; }
    [[nodiscard]] const glm::vec3& volume_max() const { return volume_max_; }
    [[nodiscard]] uint32_t resolution() const { return resolution_; }

    /// Asks for a slice through the field across `axis` (0 X, 1 Y, 2 Z) at
    /// `slice_pos` (0..1 through the volume), drawn this frame once the field
    /// is composed. The SDF debug window asks every frame it is open.
    /// `texel`, when set, is the slice texel under the mouse, whose distance
    /// hovered_distance() reports a few frames later.
    void request_slice(uint32_t axis, float slice_pos, std::optional<glm::uvec2> texel = {});
    /// The slice to show, and the nearest sampler to show it with, so each
    /// texel stays a square; none until the first slice has been asked for.
    [[nodiscard]] gpu::Texture slice_texture() const { return slice_texture_; }
    [[nodiscard]] gpu::Sampler slice_sampler() const { return slice_sampler_; }
    /// Texels across and down the slice.
    static constexpr uint32_t SLICE_SIZE = 256;
    /// The distance in metres at the texel last asked about, as the GPU
    /// wrote it MAX_FRAMES_IN_FLIGHT frames earlier; none while nothing is
    /// hovered or nothing has been read yet.
    [[nodiscard]] std::optional<float> hovered_distance() const { return hovered_distance_; }
    /// Metres either side of zero the slice draws as the surface.
    [[nodiscard]] float slice_surface() const;

    // Per-model SDF metadata
    struct MeshSdfEntry {
        uint32_t offset;      // in int16 units
        uint32_t resolution;
        glm::vec3 sdf_min;
        glm::vec3 sdf_max;
        glm::vec3 mesh_min;
        glm::vec3 mesh_max;
        float max_dist;
    };

    // Lookup a registered model's SDF entry (returns nullptr if not found)
    [[nodiscard]] const MeshSdfEntry* model_entry(uintptr_t model_id) const;

private:
    /// Records the outstanding region updates, or a full rebuild, with frame
    /// slot `frame_index`'s instance list.
    void compose(gpu::CommandList& cmd, uint32_t frame_index);
    /// Draws the slice asked for, into the slot's part of the distances.
    void draw_slice(gpu::CommandList& cmd, uint32_t frame_index);
    /// The slice's texture, readback and pipeline, made at the first
    /// request; false when they could not be, which is reported.
    [[nodiscard]] bool make_slice();

    gpu::Device* device_{nullptr};

    gpu::Owned<gpu::Texture> volume_;
    gpu::Owned<gpu::Texture> index_volume_;
    gpu::Sampler volume_sampler_{};
    gpu::Sampler index_sampler_{};
    gpu::Owned<gpu::ComputePipeline> composite_pipeline_;

    // Per-mesh SDF data packed into a single SSBO, written by the CPU
    gpu::Owned<gpu::Buffer> mesh_sdf_buffer_;
    uint32_t mesh_sdf_offset_{0}; // next free offset in int16 units

    // Per-instance metadata, host-written when a compose is recorded, so one
    // per frame in flight: a compose still running on the GPU must not read
    // the next frame's instance list.
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> instance_buffers_;

    // Each slot's materials, rewritten every frame from the gather, so one
    // table per frame in flight; `slots_` is the CPU copy, by slot.
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> slot_tables_;
    std::vector<GpuSdfSlot> slots_;
    // Slots given back by objects that left, handed out again first.
    std::vector<uint32_t> free_slots_;
    [[nodiscard]] uint32_t acquire_slot();

    // Volume config
    glm::vec3 volume_min_{-10.0f};
    glm::vec3 volume_max_{10.0f};
    uint32_t resolution_{DEFAULT_RESOLUTION};
    /// Whether the field has been composed at all: before, it holds only the
    /// empty distance it was made with, and the slice has nothing to show.
    bool composed_{false};

    std::unordered_map<uintptr_t, MeshSdfEntry> model_sdf_entries_;
    std::unordered_map<const TerrainAsset*, TerrainSdfEntry> terrain_sdf_entries_;

    // What the field currently holds, per placed mesh, so the next gather
    // can tell what moved or vanished.
    struct TrackedInstance {
        AABB bounds;
        uint32_t grid_offset;
        bool seen;
        uint32_t slot;      // NO_SDF_SLOT when every slot was taken
    };
    std::unordered_map<uint64_t, TrackedInstance> tracked_;

    // This frame's instances and their world bounds, parallel arrays
    std::vector<GpuSdfInstance> gathered_;
    std::vector<AABB> gathered_bounds_;

    // World-space boxes whose voxels must be recomputed, each already grown
    // by max_distance(). Merged where they overlap.
    std::vector<AABB> dirty_regions_;
    bool full_rebuild_{true};

    [[nodiscard]] glm::vec3 voxel_size() const;
    void add_dirty_region(AABB region);

    // Slice visualization
    gpu::Owned<gpu::ComputePipeline> slice_pipeline_;
    gpu::Owned<gpu::Texture> slice_texture_;
    gpu::Sampler slice_sampler_{};
    bool slice_requested_{false};
    uint32_t slice_axis_{1};
    float slice_pos_{0.5f};
    std::optional<glm::uvec2> slice_texel_{};
    std::optional<float> hovered_distance_{};
    // MAX_FRAMES_IN_FLIGHT slices of distances, read back by the CPU.
    gpu::Owned<gpu::Buffer> slice_distances_;
    std::array<bool, MAX_FRAMES_IN_FLIGHT> slice_written_{};
};

} // namespace fjell
