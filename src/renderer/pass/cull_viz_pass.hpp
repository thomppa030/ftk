#pragma once

#include "renderer/buffer.hpp"
#include "renderer/descriptor.hpp"
#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

namespace fjell {

class Device;
struct PassResources;
struct FrameContext;

class CullVizPass {
public:
    Result<> create(Device& device, const PassResources& resources);
    void destroy();

    // Record the viz: renders all draws, colored green/red by visibility
    void record(const FrameContext& ctx, VkBuffer visibility_buffer,
                VkDeviceSize visibility_size);

    void resize(uint32_t width, uint32_t height);
    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();

private:
    void create_image(uint32_t width, uint32_t height);
    void destroy_image();

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};

    VkImage image_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
    VkImage depth_image_{VK_NULL_HANDLE};
    VkDeviceMemory depth_memory_{VK_NULL_HANDLE};
    VkImageView depth_view_{VK_NULL_HANDLE};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    // Visibility buffer descriptor (set 4)
    std::unique_ptr<DescriptorSetLayout> vis_set_layout_;
    std::unique_ptr<DescriptorPool> vis_pool_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> vis_descriptor_sets_{};

    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks_{nullptr};
    bool rendered_once_{false};
};

} // namespace fjell
