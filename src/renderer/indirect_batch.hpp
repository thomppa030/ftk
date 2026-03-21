#pragma once

#include "renderer/buffer.hpp"
#include "renderer/pass/cull_pass.hpp"
#include "renderer/renderer_constants.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

struct DrawCommand;
struct MegaMeshEntry;
class Model;

struct GpuDrawData {
    glm::mat4 model;
    uint32_t material_id;
    uint32_t meshlet_offset;
    uint32_t meshlet_count;
    uint32_t vertex_offset;
};

static_assert(sizeof(GpuDrawData) == 80, "GpuDrawData must be 80 bytes for std430");

// A contiguous range of indirect draw commands sharing the same pipeline.
struct PipelineDrawRange {
    uint32_t pipeline_id{0};
    uint32_t first_draw{0};   // offset into indirect buffer (in commands)
    uint32_t draw_count{0};
};

class IndirectBatch {
public:
    IndirectBatch(VkDevice device, VkPhysicalDevice physical_device);

    void build(const std::vector<DrawCommand>& opaque_commands,
               const std::vector<DrawCommand>& transparent_commands,
               uint32_t frame_index);

    void draw(VkCommandBuffer cmd, uint32_t frame_index) const;

    [[nodiscard]] VkBuffer draw_data_buffer(uint32_t frame_index) const {
        return draw_data_buffers_[frame_index]->handle();
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

    [[nodiscard]] VkBuffer mesh_indirect_buffer(uint32_t frame_index) const {
        return mesh_indirect_buffers_[frame_index] ? mesh_indirect_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkBuffer mesh_count_buffer(uint32_t frame_index) const {
        return mesh_count_buffers_[frame_index] ? mesh_count_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }

    [[nodiscard]] VkBuffer pipeline_bin_buffer(uint32_t frame_index) const {
        return pipeline_bin_buffers_[frame_index] ? pipeline_bin_buffers_[frame_index]->handle() : VK_NULL_HANDLE;
    }
    [[nodiscard]] VkDeviceSize pipeline_bin_size(uint32_t frame_index) const {
        return opaque_counts_[frame_index] * sizeof(uint32_t);
    }

    [[nodiscard]] uint32_t draw_meshlet_count(uint32_t frame_index, uint32_t draw_index) const {
        auto* data = static_cast<const GpuDrawData*>(draw_data_buffers_[frame_index]->mapped());
        return data[draw_index].meshlet_count;
    }

    [[nodiscard]] const GpuDrawAABB* aabb_data(uint32_t frame_index) const {
        return aabb_data_[frame_index].empty() ? nullptr : aabb_data_[frame_index].data();
    }

private:
    void ensure_capacity(uint32_t frame_index, uint32_t count);

    VkDevice device_;
    VkPhysicalDevice physical_device_;

    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> indirect_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> draw_data_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> mesh_indirect_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> mesh_count_buffers_;
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> capacities_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> opaque_counts_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> draw_data_counts_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> total_triangles_{};
    std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> total_meshlet_instances_{};
    std::array<std::vector<PipelineDrawRange>, MAX_FRAMES_IN_FLIGHT> pipeline_ranges_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES_IN_FLIGHT> pipeline_bin_buffers_;
    std::array<std::vector<GpuDrawAABB>, MAX_FRAMES_IN_FLIGHT> aabb_data_;
};

} // namespace fjell
