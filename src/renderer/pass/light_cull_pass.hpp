#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class LightCullPass {
public:
    void create(VkDevice device, VkDescriptorSetLayout global_set_layout);
    void destroy();
    void dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                  uint32_t width, uint32_t height);

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace fjell
