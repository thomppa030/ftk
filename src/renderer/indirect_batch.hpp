#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class AnimationSystem;
struct DrawCommand;
struct MegaMeshEntry;
class Model;

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
    uint32_t _pad0{0};
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
    uint32_t indirect_cmd_index;     // which VkDrawIndexedIndirectCommand to patch
    uint32_t _pad;
};
static_assert(sizeof(GpuInstancedDraw) == 48, "GpuInstancedDraw must be 48 bytes for std430");

// A contiguous range of indirect draw commands sharing the same pipeline.
struct PipelineDrawRange {
    uint32_t pipeline_id{0};
    uint32_t first_draw{0};   // offset into indirect buffer (in commands)
    uint32_t draw_count{0};
    bool skinned{false};
};

class IndirectBatch {
public:
    /// `gpu` is the GPU interface's device, which is handed the buffers the
    /// frame graph tracks.
    IndirectBatch(gpu::Device& gpu, VkDevice device, VmaAllocator allocator);

    void build(const std::vector<DrawCommand>& opaque_commands,
               const std::vector<DrawCommand>& transparent_commands,
               uint32_t frame_index,
               const glm::mat4& projection = glm::mat4(1.0f),
               const glm::vec3& camera_pos = glm::vec3(0.0f),
               AnimationSystem* animation_system = nullptr,
               bool is_shadow_batch = false);

    void draw(VkCommandBuffer cmd, uint32_t frame_index) const;

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

    [[nodiscard]] VkBuffer draw_data_buffer(uint32_t frame_index) const {
        return draw_data_buffers_[frame_index]->handle();
    }
    /// The draw data as the GPU interface knows it.
    [[nodiscard]] gpu::Buffer shared_draw_data_buffer(uint32_t frame_index) const {
        return draw_data_buffers_[frame_index] ? draw_data_buffers_[frame_index]->shared() : gpu::Buffer{};
    }
    [[nodiscard]] VkDeviceSize draw_data_size(uint32_t frame_index) const {
        return draw_data_counts_[frame_index] * sizeof(GpuDrawData);
    }
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

    [[nodiscard]] VkBuffer indirect_buffer(uint32_t frame_index) const {
        return indirect_buffers_[frame_index]->handle();
    }
    /// The indirect commands as the GPU interface knows them.
    [[nodiscard]] gpu::Buffer shared_indirect_buffer(uint32_t frame_index) const {
        return indirect_buffers_[frame_index] ? indirect_buffers_[frame_index]->shared() : gpu::Buffer{};
    }

    [[nodiscard]] VkBuffer mesh_indirect_buffer(uint32_t frame_index) const {
        return mesh_indirect_buffers_[frame_index] ? mesh_indirect_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkBuffer mesh_count_buffer(uint32_t frame_index) const {
        return mesh_count_buffers_[frame_index] ? mesh_count_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }
    /// The mesh-shader draw commands, one gpu::DrawMeshTasksArgs per static
    /// draw, and each pipeline range's draw count, as the GPU interface knows
    /// them.
    [[nodiscard]] gpu::Buffer shared_mesh_indirect_buffer(uint32_t frame_index) const {
        return mesh_indirect_buffers_[frame_index] ? mesh_indirect_buffers_[frame_index]->shared() : gpu::Buffer{};
    }
    [[nodiscard]] gpu::Buffer shared_mesh_count_buffer(uint32_t frame_index) const {
        return mesh_count_buffers_[frame_index] ? mesh_count_buffers_[frame_index]->shared() : gpu::Buffer{};
    }
    /// The most task groups any of those commands asks for.
    [[nodiscard]] uint32_t most_mesh_task_groups(uint32_t frame_index) const {
        return most_mesh_task_groups_[frame_index];
    }

    [[nodiscard]] VkBuffer pipeline_bin_buffer(uint32_t frame_index) const {
        return pipeline_bin_buffers_[frame_index] ? pipeline_bin_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }
    /// Each draw's pipeline bin as the GPU interface knows it.
    [[nodiscard]] gpu::Buffer shared_pipeline_bin_buffer(uint32_t frame_index) const {
        return pipeline_bin_buffers_[frame_index] ? pipeline_bin_buffers_[frame_index]->shared() : gpu::Buffer{};
    }
    [[nodiscard]] VkDeviceSize pipeline_bin_size(uint32_t frame_index) const {
        return opaque_counts_[frame_index] * sizeof(uint32_t);
    }

    [[nodiscard]] uint32_t draw_meshlet_count(uint32_t frame_index, uint32_t draw_index) const {
        auto* data = static_cast<const GpuDrawData*>(draw_data_buffers_[frame_index]->mapped());
        return data[draw_index].meshlet_count;
    }

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

    [[nodiscard]] VkBuffer instance_transform_buffer(uint32_t frame_index) const {
        return instance_transform_buffers_[frame_index]
                   ? instance_transform_buffers_[frame_index]->handle()
                   : VK_NULL_HANDLE;
    }
    /// The instance transforms as the GPU interface knows them.
    [[nodiscard]] gpu::Buffer shared_instance_transform_buffer(uint32_t frame_index) const {
        return instance_transform_buffers_[frame_index]
                   ? instance_transform_buffers_[frame_index]->shared()
                   : gpu::Buffer{};
    }
    [[nodiscard]] VkDeviceSize instance_transform_size(uint32_t frame_index) const {
        return static_cast<VkDeviceSize>(instance_transform_counts_[frame_index]) * sizeof(glm::mat4);
    }
    [[nodiscard]] uint32_t instance_transform_count(uint32_t frame_index) const {
        return instance_transform_counts_[frame_index];
    }

    [[nodiscard]] VkBuffer visible_instance_buffer(uint32_t frame_index) const {
        return visible_instance_buffers_[frame_index]
                   ? visible_instance_buffers_[frame_index]->handle()
                   : VK_NULL_HANDLE;
    }
    /// The visible instances as the GPU interface knows them.
    [[nodiscard]] gpu::Buffer shared_visible_instance_buffer(uint32_t frame_index) const {
        return visible_instance_buffers_[frame_index]
                   ? visible_instance_buffers_[frame_index]->shared()
                   : gpu::Buffer{};
    }
    [[nodiscard]] VkDeviceSize visible_instance_size(uint32_t frame_index) const {
        return static_cast<VkDeviceSize>(instance_transform_counts_[frame_index]) * sizeof(glm::mat4);
    }

    [[nodiscard]] VkBuffer instanced_draw_buffer(uint32_t frame_index) const {
        return instanced_draw_buffers_[frame_index]
                   ? instanced_draw_buffers_[frame_index]->handle()
                   : VK_NULL_HANDLE;
    }
    /// The instanced draws as the GPU interface knows them.
    [[nodiscard]] gpu::Buffer shared_instanced_draw_buffer(uint32_t frame_index) const {
        return instanced_draw_buffers_[frame_index]
                   ? instanced_draw_buffers_[frame_index]->shared()
                   : gpu::Buffer{};
    }
    [[nodiscard]] VkDeviceSize instanced_draw_size(uint32_t frame_index) const {
        return static_cast<VkDeviceSize>(instanced_draw_counts_[frame_index]) * sizeof(GpuInstancedDraw);
    }
    [[nodiscard]] uint32_t instanced_draw_count(uint32_t frame_index) const {
        return instanced_draw_counts_[frame_index];
    }

    [[nodiscard]] VkBuffer spline_segment_buffer(uint32_t frame_index) const {
        return spline_segment_buffers_[frame_index]
                   ? spline_segment_buffers_[frame_index]->handle()
                   : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkDeviceSize spline_segment_size(uint32_t frame_index) const {
        return static_cast<VkDeviceSize>(spline_segment_counts_[frame_index]) * sizeof(GpuSplineSegment);
    }
    [[nodiscard]] uint32_t spline_segment_count(uint32_t frame_index) const {
        return spline_segment_counts_[frame_index];
    }

private:
    void ensure_capacity(uint32_t frame_index, uint32_t count);
    void ensure_instance_capacity(uint32_t frame_index, uint32_t count);
    void ensure_instanced_draw_capacity(uint32_t frame_index, uint32_t count);
    void ensure_spline_segment_capacity(uint32_t frame_index, uint32_t count);

    // Keeps an outgoing buffer alive until the frames using it are done
    // instead of freeing it inline: a pending command buffer may still read
    // it.
    void retire(std::unique_ptr<Buffer> buffer);

    gpu::Device& gpu_;
    VkDevice device_;
    VmaAllocator allocator_;

    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> indirect_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> draw_data_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> mesh_indirect_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> mesh_count_buffers_;
    std::array<std::vector<NodeDraws>, MAX_FRAMES_IN_FLIGHT> node_draws_;

    // What the last build picked LODs from: its camera, and its projection's
    // vertical scale (0 without a projection, which picks LOD 0).
    glm::vec3 lod_camera_pos_{0.0f};
    float lod_proj_scale_{0.0f};

    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> capacities_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> opaque_counts_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> draw_data_counts_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> total_triangles_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> total_meshlet_instances_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> most_mesh_task_groups_{};
    std::array<std::vector<PipelineDrawRange>, MAX_FRAMES_IN_FLIGHT> pipeline_ranges_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> pipeline_bin_buffers_;

    // CPU-side staging buffers (avoid scattered writes to write-combined GPU memory)
    std::vector<VkDrawIndexedIndirectCommand> staged_indirect_;
    std::vector<GpuDrawData> staged_draw_data_;

    // Per-draw scratch for instanced LOD bucketing — reused across draws to
    // avoid nested std::vector allocations in the hot loop.
    std::vector<uint8_t> staged_bucket_lods_;
    std::vector<glm::mat4> staged_bucket_worlds_;

    // Per-frame skinned draw data indices (draw_index into GpuDrawData SSBO)
    std::array<std::vector<uint32_t>, MAX_FRAMES_IN_FLIGHT> skinned_draw_indices_;

    // Per-frame packed instance transforms (world-space, concatenated for all instanced draws)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> instance_transform_buffers_;
    // GPU-written compacted visible instances — vertex path reads this
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> visible_instance_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instance_transform_capacities_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instance_transform_counts_{};
    std::vector<glm::mat4> staged_instance_transforms_;

    // Per-frame per-instanced-draw descriptors (local AABB + indirect cmd index)
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> instanced_draw_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instanced_draw_capacities_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> instanced_draw_counts_{};
    std::vector<GpuInstancedDraw> staged_instanced_draws_;

    // Per-frame packed spline-mesh per-instance segment records
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> spline_segment_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> spline_segment_capacities_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> spline_segment_counts_{};
    std::vector<GpuSplineSegment> staged_spline_segments_;
};

} // namespace fjell
