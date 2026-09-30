#pragma once

#include "renderer/gpu/descriptor.hpp"
#include "renderer/renderer_constants.hpp"

#include <gpu/buffer.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace fjell {

namespace gpu {
class Device;
}

class DrawDataDescriptor {
public:
    DrawDataDescriptor() = default;

    /// `gpu` is the device the buffers bound here were made by.
    void create(VkDevice device, gpu::Device& gpu);
    void destroy();

    // Each binds the first `size` bytes of a buffer into the frame slot's
    // set; nothing while the buffer is unset or `size` is 0.
    void update(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);
    void update_bones(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);
    void update_instance_transforms(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);
    void update_visible_instance_transforms(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);
    void update_spline_segments(gpu::Buffer buffer, uint64_t size, uint32_t frame_index);

    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_->handle(); }
    /// The bindings the layout was made from, for the GPU interface's shared
    /// layout.
    [[nodiscard]] std::span<const VkDescriptorSetLayoutBinding> layout_bindings() const {
        return layout_bindings_;
    }
    [[nodiscard]] VkDescriptorSet descriptor_set(uint32_t frame_index) const {
        return descriptor_sets_[frame_index];
    }

private:
    void write(uint32_t binding, VkBuffer buffer, VkDeviceSize size, uint32_t frame_index);

    VkDevice device_{VK_NULL_HANDLE};
    gpu::Device* gpu_{nullptr};
    std::unique_ptr<DescriptorSetLayout> layout_;
    std::array<VkDescriptorSetLayoutBinding, 5> layout_bindings_{};
    std::unique_ptr<DescriptorPool> pool_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptor_sets_{};
};

} // namespace fjell
