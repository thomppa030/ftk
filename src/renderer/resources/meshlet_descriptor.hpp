#pragma once

#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/descriptor.hpp"
#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace fjell {

class MeshletBuffer;
class MegaBuffer;

class MeshletDescriptor {
public:
    MeshletDescriptor() = default;

    void create(VkDevice device, VmaAllocator allocator = VK_NULL_HANDLE);
    void destroy();

    void update(const MeshletBuffer& meshlet_buf, const MegaBuffer& mega_buf,
                uint32_t frame_index,
                VkBuffer visible_buffer = VK_NULL_HANDLE,
                VkDeviceSize visible_size = 0);


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
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    std::unique_ptr<DescriptorSetLayout> layout_;
    std::array<VkDescriptorSetLayoutBinding, 6> layout_bindings_{};
    std::unique_ptr<DescriptorPool> pool_;
    std::unique_ptr<Buffer> dummy_buffer_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptor_sets_{};
};

} // namespace fjell
