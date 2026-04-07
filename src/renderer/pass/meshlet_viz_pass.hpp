#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace fjell {

class MeshletVizPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    [[nodiscard]] VkDescriptorSet imgui_texture();
    void invalidate_imgui_texture();
    void resize(uint32_t width, uint32_t height);

private:
    void create_image(uint32_t width, uint32_t height);
    void destroy_image();

    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};

    VkImage image_{VK_NULL_HANDLE};
    VmaAllocation memory_{VK_NULL_HANDLE};
    VkImageView view_{VK_NULL_HANDLE};
    VkImage depth_image_{VK_NULL_HANDLE};
    VmaAllocation depth_memory_{VK_NULL_HANDLE};
    VkImageView depth_view_{VK_NULL_HANDLE};
    VkFormat depth_format_{VK_FORMAT_UNDEFINED};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    VkDescriptorSet imgui_descriptor_{VK_NULL_HANDLE};

    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout meshlet_layout_{VK_NULL_HANDLE};
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks_{nullptr};
    PFN_vkCmdDrawMeshTasksIndirectCountEXT draw_mesh_tasks_indirect_count_{nullptr};
    bool rendered_once_{false};
};

} // namespace fjell
