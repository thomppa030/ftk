#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/resources/mega_buffer.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace fjell {

class Model;
struct GpuDrawData;

/// Per-RT-instance data, accessible in closest-hit shader via gl_InstanceID.
struct GpuRtInstance {
    uint32_t material_id;
    uint32_t first_index;    // into global index buffer
    uint32_t vertex_offset;  // into global vertex buffer
    uint32_t pad;
};

/// One bottom-level acceleration structure (BLAS) per unique Model.
struct Blas {
    VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
    std::unique_ptr<Buffer> buffer;
    VkDeviceAddress device_address{0};
    uint32_t first_index{0};    // LOD 0 first_index in MegaBuffer
    uint32_t vertex_offset{0};  // LOD 0 vertex_offset in MegaBuffer
};

/// Manages BLAS + TLAS for DDGI ray tracing.
class AccelStructureManager {
public:
    AccelStructureManager(Device& device, VmaAllocator allocator,
                          VkCommandPool command_pool, VkQueue queue);
    ~AccelStructureManager();

    AccelStructureManager(const AccelStructureManager&) = delete;
    AccelStructureManager& operator=(const AccelStructureManager&) = delete;
    AccelStructureManager(AccelStructureManager&&) = delete;
    AccelStructureManager& operator=(AccelStructureManager&&) = delete;

    /// Build or retrieve BLAS for a Model. Call during model load.
    void ensure_blas(const Model* model, const MegaBuffer& mega);

    /// Rebuild TLAS from this frame's draw instances.
    /// Call once per frame before DDGI RT tracing.
    void build_tlas(VkCommandBuffer cmd,
                    const GpuDrawData* draw_data, uint32_t draw_count,
                    const std::vector<const Model*>& draw_models);

    [[nodiscard]] VkAccelerationStructureKHR tlas() const { return tlas_handle_; }
    [[nodiscard]] bool tlas_valid() const { return tlas_handle_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkBuffer instance_data_buffer() const { return rt_instance_data_ ? rt_instance_data_->handle() : VK_NULL_HANDLE; }
    [[nodiscard]] uint32_t instance_data_count() const { return rt_instance_count_; }

private:
    VkDeviceAddress buffer_address(VkBuffer buffer) const;
    void build_blas(const Model* model, const MegaBuffer& mega);

    Device& device_;
    VkDevice vk_device_;
    VmaAllocator allocator_;
    VkCommandPool command_pool_;
    VkQueue queue_;

    std::unordered_map<const Model*, Blas> blas_map_;

    // TLAS
    VkAccelerationStructureKHR tlas_handle_{VK_NULL_HANDLE};
    std::unique_ptr<Buffer> tlas_buffer_;
    std::unique_ptr<Buffer> tlas_scratch_;
    std::unique_ptr<Buffer> instance_buffer_;
    std::unique_ptr<Buffer> rt_instance_data_;
    uint32_t instance_capacity_{0};
    uint32_t rt_instance_count_{0};
};

} // namespace fjell
