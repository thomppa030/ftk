#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

class MeshShaderDepthPass : public RenderPass {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    // Skinned depth: vertex shader pipeline (skinned meshes can't use mesh shaders)
    VkPipelineLayout skinned_layout_{VK_NULL_HANDLE};
    VkPipeline skinned_pipeline_{VK_NULL_HANDLE};

    VkDescriptorSetLayout draw_data_layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout meshlet_layout_{VK_NULL_HANDLE};
    PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks_{nullptr};
    PFN_vkCmdDrawMeshTasksIndirectCountEXT draw_mesh_tasks_indirect_count_{nullptr};
};

} // namespace fjell
