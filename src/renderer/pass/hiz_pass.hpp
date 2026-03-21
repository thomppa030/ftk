#pragma once

#include "renderer/buffer.hpp"
#include "renderer/descriptor.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fjell {

class Device;

class HizPass {
public:
    static constexpr uint32_t MAX_HIZ_MIPS = 12;

    void create(VkDevice device, VkPhysicalDevice physical_device);
    void destroy();
    void resize(uint32_t width, uint32_t height);

    // Build the Hi-Z pyramid from the depth buffer
    void dispatch(VkCommandBuffer cmd, VkImageView depth_view,
                  uint32_t depth_width, uint32_t depth_height);

    [[nodiscard]] VkImageView sampled_view() const { return hiz_sampled_view_; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] uint32_t mip_count() const { return hiz_mip_count_; }
    [[nodiscard]] bool ready() const { return hiz_image_ != VK_NULL_HANDLE; }

private:
    void create_image(uint32_t width, uint32_t height);
    void destroy_image();

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    // Hi-Z pyramid image (R32F with mipchain)
    VkImage hiz_image_{VK_NULL_HANDLE};
    VkDeviceMemory hiz_memory_{VK_NULL_HANDLE};
    VkImageView hiz_sampled_view_{VK_NULL_HANDLE}; // all mips, for sampling
    std::array<VkImageView, MAX_HIZ_MIPS> hiz_mip_views_{}; // per-mip, for storage writes
    uint32_t hiz_mip_count_{0};
    uint32_t hiz_width_{0};
    uint32_t hiz_height_{0};

    VkSampler sampler_{VK_NULL_HANDLE};

    // Pipeline
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    // Per-mip descriptor sets
    std::unique_ptr<DescriptorSetLayout> set_layout_;
    std::unique_ptr<DescriptorPool> pool_;
    std::array<VkDescriptorSet, MAX_HIZ_MIPS> mip_sets_{};
};

} // namespace fjell
