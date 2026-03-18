#pragma once

#include "renderer/descriptor.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

namespace fjell {

class DrawDataDescriptor {
public:
    static constexpr uint32_t MAX_FRAMES = 2;

    DrawDataDescriptor() = default;

    void create(VkDevice device);
    void destroy();

    void update(VkBuffer buffer, VkDeviceSize size, uint32_t frame_index);

    [[nodiscard]] VkDescriptorSetLayout layout() const { return layout_->handle(); }
    [[nodiscard]] VkDescriptorSet descriptor_set(uint32_t frame_index) const {
        return descriptor_sets_[frame_index];
    }

private:
    VkDevice device_{VK_NULL_HANDLE};
    std::unique_ptr<DescriptorSetLayout> layout_;
    std::unique_ptr<DescriptorPool> pool_;
    std::array<VkDescriptorSet, MAX_FRAMES> descriptor_sets_{};
};

} // namespace fjell
