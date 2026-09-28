#pragma once

#include "renderer/gpu/descriptor.hpp"
#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace fjell {

class DrawDataDescriptor {
public:
    DrawDataDescriptor() = default;

    void create(VkDevice device);
    void destroy();

    void update(VkBuffer buffer, VkDeviceSize size, uint32_t frame_index);
    void update_bones(VkBuffer buffer, VkDeviceSize size, uint32_t frame_index);
    void update_instance_transforms(VkBuffer buffer, VkDeviceSize size, uint32_t frame_index);
    void update_visible_instance_transforms(VkBuffer buffer, VkDeviceSize size, uint32_t frame_index);
    void update_spline_segments(VkBuffer buffer, VkDeviceSize size, uint32_t frame_index);

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
    std::unique_ptr<DescriptorSetLayout> layout_;
    std::array<VkDescriptorSetLayoutBinding, 5> layout_bindings_{};
    std::unique_ptr<DescriptorPool> pool_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptor_sets_{};
};

} // namespace fjell
