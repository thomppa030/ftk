#pragma once

#include "renderer/buffer.hpp"

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

class IndirectBatch {
public:
    static constexpr uint32_t MAX_FRAMES = 2;

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

    [[nodiscard]] uint32_t total_triangles(uint32_t frame_index) const {
        return total_triangles_[frame_index];
    }

private:
    void ensure_capacity(uint32_t frame_index, uint32_t count);

    VkDevice device_;
    VkPhysicalDevice physical_device_;

    std::array<std::unique_ptr<Buffer>, MAX_FRAMES> indirect_buffers_;
    std::array<std::unique_ptr<Buffer>, MAX_FRAMES> draw_data_buffers_;
    std::array<uint32_t, MAX_FRAMES> capacities_{};
    std::array<uint32_t, MAX_FRAMES> opaque_counts_{};
    std::array<uint32_t, MAX_FRAMES> draw_data_counts_{};
    std::array<uint32_t, MAX_FRAMES> total_triangles_{};
};

} // namespace fjell
