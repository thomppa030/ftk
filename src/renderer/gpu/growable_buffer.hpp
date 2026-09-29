#pragma once

#include "renderer/gpu/buffer.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fjell {

class Device;
class UploadContext;

/// GPU buffer that grows without invalidating its handle or device address.
///
/// Sparse mode (device supports sparseBinding): the buffer is created with a
/// large virtual size and no memory behind it; ensure_capacity() binds
/// additional pages in place via vkQueueBindSparse. Nothing moves — baked
/// device addresses (BLAS geometry) and descriptor bindings stay valid
/// forever, and growth copies nothing.
///
/// Fallback mode: classic allocate-bigger / GPU-copy, the old buffer kept
/// until the frames using it are done. The handle changes and generation()
/// bumps so consumers that baked the old address know to rebuild.
class GrowableBuffer {
public:
    GrowableBuffer(Device& device, VmaAllocator allocator,
                   UploadContext& upload, gpu::Device& gpu,
                   VkBufferUsageFlags usage,
                   VkDeviceSize initial_capacity,
                   VkDeviceSize virtual_capacity,
                   std::string debug_name);
    ~GrowableBuffer();

    GrowableBuffer(const GrowableBuffer&) = delete;
    GrowableBuffer& operator=(const GrowableBuffer&) = delete;
    GrowableBuffer(GrowableBuffer&&) = delete;
    GrowableBuffer& operator=(GrowableBuffer&&) = delete;

    /// Make at least `needed` bytes usable. Sparse: binds more pages (the
    /// handle stays put). Fallback: reallocates, schedules a GPU copy of the
    /// old content through the upload context, retires the old buffer.
    void ensure_capacity(VkDeviceSize needed);

    [[nodiscard]] VkBuffer handle() const {
        return sparse_buffer_ != VK_NULL_HANDLE ? sparse_buffer_ : fallback_->handle();
    }
    [[nodiscard]] VkDeviceSize capacity() const { return capacity_; }

    /// Hands the buffer to the GPU interface's device, as `Buffer::share`
    /// does; a growth that replaces the buffer hands on the new one.
    void share();

    /// The buffer as the GPU interface knows it; invalid until shared.
    [[nodiscard]] gpu::Buffer shared() const {
        return sparse_buffer_ != VK_NULL_HANDLE ? sparse_shared_.get() : fallback_->shared();
    }

    /// Bumps every time the handle changed (fallback growth only — stays 0
    /// in sparse mode). Consumers that baked the device address stamp this
    /// and rebuild when it moves.
    [[nodiscard]] uint32_t generation() const { return generation_; }

private:
    void bind_pages(VkDeviceSize target);

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    UploadContext& upload_;
    gpu::Device& gpu_;
    VkBufferUsageFlags usage_{0};
    VkDeviceSize virtual_capacity_{0};
    std::string name_;

    VkDeviceSize capacity_{0};
    uint32_t generation_{0};

    // Sparse mode
    VkBuffer sparse_buffer_{VK_NULL_HANDLE};
    VkQueue bind_queue_{VK_NULL_HANDLE};
    VkFence bind_fence_{VK_NULL_HANDLE};
    VkMemoryRequirements memory_requirements_{};
    std::vector<VmaAllocation> pages_;

    // Fallback mode
    std::unique_ptr<Buffer> fallback_;

    bool shared_{false};
    gpu::Owned<gpu::Buffer> sparse_shared_;
};

} // namespace fjell
