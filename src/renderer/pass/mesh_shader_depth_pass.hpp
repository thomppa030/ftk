#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

class MeshShaderDepthPass : public RenderPass {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    void set_draw_data_layout(VkDescriptorSetLayout layout) { draw_data_layout_ = layout; }
    void set_meshlet_layout(VkDescriptorSetLayout layout) { meshlet_layout_ = layout; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout meshlet_layout_{VK_NULL_HANDLE};
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks_{nullptr};
};

} // namespace fjell
