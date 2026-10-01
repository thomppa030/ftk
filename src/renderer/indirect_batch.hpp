#pragma once

#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/render_encoder.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace fjell {

class AnimationSystem;
struct DrawCommand;
namespace gpu {
class Frame;
}
struct MegaMeshEntry;
class Model;
class PipelineCache;

/// The draw's surfaces keep what faces away from the camera (a material that
/// culls nothing, or front faces): the meshlet cull must not drop meshlets
/// that all face away. Matches fjell_draw_data.glsl.
constexpr uint32_t DRAW_FLAG_KEEPS_BACK_FACES = 1U;

struct GpuDrawData {
    glm::mat4 model;
    glm::mat4 prev_model;           // previous frame's world matrix (for motion vectors)
    uint32_t material_id;
    uint32_t meshlet_offset;
    uint32_t meshlet_count;
    uint32_t vertex_offset;
    uint32_t bone_offset{0xFFFFFFFF};   // offset into bone SSBO (0xFFFFFFFF = not skinned)
    uint32_t bone_count{0};
    uint32_t instance_count{1};          // 1 = single instance; N = instanced
    uint32_t first_instance{0};          // offset into instance transform SSBO

    // Offset into the spline-segment SSBO for draw's first instance. Per-
    // instance segment = spline_segment_offset + (gl_InstanceIndex - first_instance).
    // 0xFFFFFFFF = not a spline-mesh draw (vertex stage skips deform).
    uint32_t spline_segment_offset{0xFFFFFFFF};
    /// DRAW_FLAG_* bits.
    uint32_t flags{0};
    uint32_t _pad1{0};
    uint32_t _pad2{0};
};

static_assert(sizeof(GpuDrawData) == 176, "GpuDrawData must be 176 bytes for std430");

// Per-instance spline-mesh segment record. Layout matches
// `SplineSegment` in shaders/include/fjell_spline_deform.glsl (std430).
//
// start_quat / end_quat encode the rotation-minimizing frame at each endpoint
// (xyzw, unit quaternion). The shader slerps between them per-t, which avoids
// the discontinuities a per-t Frenet frame produces at inflection points /
// sharp bends.
struct GpuSplineSegment {
    glm::vec3 start_pos;
    float     half_span;     // mesh extent along forward axis, object-space
    glm::vec3 start_tan;
    uint32_t  forward;       // 0 = X, 1 = Y, 2 = Z
    glm::vec3 end_pos;
    float     _pad0;
    glm::vec3 end_tan;
    float     arc_length;   // approximate segment length in object space
    glm::vec4 start_quat;
    glm::vec4 end_quat;
};
static_assert(sizeof(GpuSplineSegment) == 96, "GpuSplineSegment must be 96 bytes for std430");

// One per instanced draw (each mega-entry × each distinct instanced DrawCommand).
// Consumed by InstanceCullPass to cull instances against the view frustum and
// compact the indirect command's instanceCount in-place.
struct GpuInstancedDraw {
    glm::vec4 local_aabb_min;        // model local AABB min (.w unused)
    glm::vec4 local_aabb_max;        // model local AABB max (.w unused)
    uint32_t first_instance;         // offset into InstanceTransforms
    uint32_t instance_count;         // source instance count (pre-cull)
    uint32_t indirect_cmd_index;     // which indirect command's instance count to patch
    uint32_t _pad;
};
static_assert(sizeof(GpuInstancedDraw) == 48, "GpuInstancedDraw must be 48 bytes for std430");

// A contiguous range of indirect draw commands sharing the same pipeline.
struct PipelineDrawRange {
    uint32_t pipeline_id{0};
    uint32_t first_draw{0};   // offset into indirect buffer (in commands)
    uint32_t draw_count{0};
    /// The meshlets the range's draws expand to, each instance's apart, and
    /// how many of the frame's come before them: the range's segment of the
    /// meshlets the meshlet cull finds visible.
    uint32_t first_meshlet_instance{0};
    uint32_t meshlet_instances{0};
};

/// The draws one view's run makes of a render queue: indirect commands, draw
/// records and what they index, built on the CPU and written into the
/// frame's memory, so each run's are its own. The slices are indexed by the
/// frame slot that built them, and are at least one word of zeroes when a
/// build has nothing for them, so a binding of one is never empty.
class IndirectBatch {
public:
    /// The buffers the GPU writes are made through `gpu`.
    explicit IndirectBatch(gpu::Device& gpu);

    void build(gpu::Frame& frame,
               const std::vector<DrawCommand>& opaque_commands,
               const std::vector<DrawCommand>& transparent_commands,
               const glm::mat4& projection = glm::mat4(1.0f),
               const glm::vec3& camera_pos = glm::vec3(0.0f),
               AnimationSystem* animation_system = nullptr,
               bool is_shadow_batch = false,
               const PipelineCache* pipelines = nullptr);


    /// The static draws one opaque command's node got in a build: that many
    /// indirect commands and draw data entries from `first_draw` on.
    struct NodeDraws {
        uint32_t node_id{0};
        uint32_t first_draw{0};
        uint32_t draw_count{0};
    };
    /// The static draws the build for this frame slot made, command by
    /// command; a command culled whole or drawn skinned has none.
    [[nodiscard]] const std::vector<NodeDraws>& node_draws(uint32_t frame_index) const {
        return node_draws_[frame_index];
    }

    /// The LOD of `model` placed by `world` that the last build draws, as it
    /// picks one for each opaque draw: from the model's projected size seen
    /// from that build's camera. `model.lod_count()` when it is too small to
    /// draw; 0 for a build without a projection (the shadow batches).
    [[nodiscard]] uint32_t lod_for(const Model& model, const glm::mat4& world) const;

    /// One GpuDrawData per draw: the static opaque draws, then the skinned
    /// ones, then the transparent ones.
    [[nodiscard]] gpu::BufferRange draw_data(uint32_t frame_index) const { return draw_data_[frame_index]; }
    [[nodiscard]] uint32_t draw_data_count(uint32_t frame_index) const {
        return draw_data_counts_[frame_index];
    }
    [[nodiscard]] uint32_t opaque_draw_count(uint32_t frame_index) const {
        return opaque_counts_[frame_index];
    }
    [[nodiscard]] uint32_t total_meshlet_instances(uint32_t frame_index) const {
        return total_meshlet_instances_[frame_index];
    }

    [[nodiscard]] uint32_t total_triangles(uint32_t frame_index) const {
        return total_triangles_[frame_index];
    }

    [[nodiscard]] const std::vector<PipelineDrawRange>& pipeline_ranges(uint32_t frame_index) const {
        return pipeline_ranges_[frame_index];
    }

    /// One gpu::DrawIndexedArgs per static opaque draw. The instance cull
    /// patches the instance counts of the instanced ones in place.
    [[nodiscard]] gpu::BufferRange indirect_commands(uint32_t frame_index) const {
        return indirect_commands_[frame_index];
    }
    /// `count` of those from the `first`th, to draw indirect.
    [[nodiscard]] gpu::BufferRange indirect_commands(uint32_t frame_index, uint32_t first, uint32_t count) const {
        return slice(indirect_commands_[frame_index], first, count, sizeof(gpu::DrawIndexedArgs));
    }

    /// The mesh-shader draw commands, one gpu::DrawMeshTasksArgs per static
    /// draw: `count` of them from the `first`th.
    [[nodiscard]] gpu::BufferRange mesh_commands(uint32_t frame_index, uint32_t first, uint32_t count) const {
        return slice(mesh_commands_[frame_index], first, count, sizeof(gpu::DrawMeshTasksArgs));
    }
    /// The draw count of pipeline range `range`, as a draw with an indirect
    /// count reads it.
    [[nodiscard]] gpu::BufferRange mesh_count(uint32_t frame_index, uint32_t range) const {
        return slice(mesh_counts_[frame_index], range, 1, sizeof(uint32_t));
    }
    /// The most task groups any of those commands asks for.
    [[nodiscard]] uint32_t most_mesh_task_groups(uint32_t frame_index) const {
        return most_mesh_task_groups_[frame_index];
    }

    /// Each static draw's pipeline bin: the index of its pipeline range.
    [[nodiscard]] gpu::BufferRange pipeline_bins(uint32_t frame_index) const { return pipeline_bins_[frame_index]; }

    [[nodiscard]] const std::vector<uint32_t>& skinned_draw_indices(uint32_t frame_index) const {
        return skinned_draw_indices_[frame_index];
    }

    /// Index of the first transparent entry in the draw data buffer.
    /// Transparent draws are staged after the static opaque draws and the
    /// skinned opaque draws that follow them, so this is where a transparent
    /// draw's firstInstance starts counting.
    [[nodiscard]] uint32_t transparent_draw_base(uint32_t frame_index) const {
        return opaque_counts_[frame_index] +
               static_cast<uint32_t>(skinned_draw_indices_[frame_index].size());
    }

    /// Every instanced draw's world transforms, one after another.
    [[nodiscard]] gpu::BufferRange instance_transforms(uint32_t frame_index) const {
        return instance_transforms_[frame_index];
    }
    [[nodiscard]] uint32_t instance_transform_count(uint32_t frame_index) const {
        return instance_transform_counts_[frame_index];
    }

    /// The transforms the instance cull keeps, written on the GPU, as many
    /// as there are instance transforms; empty without any.
    [[nodiscard]] gpu::BufferRange visible_instances(uint32_t frame_index) const {
        if (instance_transform_counts_[frame_index] == 0) return {};
        return {visible_instance_buffers_[frame_index], 0,
                uint64_t{instance_transform_counts_[frame_index]} * sizeof(glm::mat4)};
    }

    /// One GpuInstancedDraw per instanced draw.
    [[nodiscard]] gpu::BufferRange instanced_draws(uint32_t frame_index) const {
        return instanced_draws_[frame_index];
    }
    [[nodiscard]] uint32_t instanced_draw_count(uint32_t frame_index) const {
        return instanced_draw_counts_[frame_index];
    }

    /// One GpuSplineSegment per instance of a spline mesh draw.
    [[nodiscard]] gpu::BufferRange spline_segments(uint32_t frame_index) const {
        return spline_segments_[frame_index];
    }
    [[nodiscard]] uint32_t spline_segment_count(uint32_t frame_index) const {
        return spline_segment_counts_[frame_index];
    }

private:
    template <typename T>
    using PerFrame = std::array<T, MAX_FRAMES_IN_FLIGHT>;

    /// `count` elements of `stride` bytes from the `first`th of `range`.
    [[nodiscard]] static gpu::BufferRange slice(const gpu::BufferRange& range, uint32_t first, uint32_t count,
                                                uint64_t stride) {
        return {range.buffer, range.offset + uint64_t{first} * stride, uint64_t{count} * stride};
    }

    /// Grows a frame slot's visible instance buffer to hold `count`
    /// transforms. The frames still reading the old one keep it until they
    /// are done. False, with the reason logged, when it could not be made.
    [[nodiscard]] bool ensure_visible_capacity(uint32_t frame_index, uint32_t count);

    /// Empties the build of `frame`'s slot: nothing to draw.
    void clear(gpu::Frame& frame);

    gpu::Device& gpu_;

    PerFrame<gpu::BufferRange> indirect_commands_;
    PerFrame<gpu::BufferRange> draw_data_;
    PerFrame<gpu::BufferRange> mesh_commands_;
    PerFrame<gpu::BufferRange> mesh_counts_;
    PerFrame<gpu::BufferRange> pipeline_bins_;
    PerFrame<gpu::BufferRange> instance_transforms_;
    PerFrame<gpu::BufferRange> instanced_draws_;
    PerFrame<gpu::BufferRange> spline_segments_;
    PerFrame<std::vector<NodeDraws>> node_draws_;

    // What the last build picked LODs from: its camera, and its projection's
    // vertical scale (0 without a projection, which picks LOD 0).
    glm::vec3 lod_camera_pos_{0.0f};
    float lod_proj_scale_{0.0f};

    PerFrame<uint32_t> opaque_counts_{};
    PerFrame<uint32_t> draw_data_counts_{};
    PerFrame<uint32_t> total_triangles_{};
    PerFrame<uint32_t> total_meshlet_instances_{};
    PerFrame<uint32_t> most_mesh_task_groups_{};
    PerFrame<std::vector<PipelineDrawRange>> pipeline_ranges_;

    // Built on the CPU, then written into the frame's memory in one copy
    // each.
    std::vector<gpu::DrawIndexedArgs> staged_indirect_;
    std::vector<GpuDrawData> staged_draw_data_;
    std::vector<gpu::DrawMeshTasksArgs> staged_mesh_commands_;
    std::vector<uint32_t> staged_mesh_counts_;
    std::vector<uint32_t> staged_pipeline_bins_;

    // Per-draw scratch for instanced LOD bucketing — reused across draws to
    // avoid nested std::vector allocations in the hot loop.
    std::vector<uint8_t> staged_bucket_lods_;
    std::vector<glm::mat4> staged_bucket_worlds_;

    // Per-frame skinned draw data indices (draw_index into GpuDrawData SSBO)
    PerFrame<std::vector<uint32_t>> skinned_draw_indices_;

    // Packed instance transforms (world-space, concatenated for all
    // instanced draws), and the GPU-written compacted visible ones the
    // vertex path reads.
    PerFrame<gpu::Owned<gpu::Buffer>> visible_instance_buffers_;
    PerFrame<uint32_t> visible_instance_capacities_{};
    PerFrame<uint32_t> instance_transform_counts_{};
    std::vector<glm::mat4> staged_instance_transforms_;

    // Per-instanced-draw descriptors (local AABB + indirect cmd index)
    PerFrame<uint32_t> instanced_draw_counts_{};
    std::vector<GpuInstancedDraw> staged_instanced_draws_;

    // Packed spline-mesh per-instance segment records
    PerFrame<uint32_t> spline_segment_counts_{};
    std::vector<GpuSplineSegment> staged_spline_segments_;
};

} // namespace fjell
