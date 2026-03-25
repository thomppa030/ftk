#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>

namespace fjell {

// Depth-only pre-pass. Writes depth buffer with a minimal vertex shader,
// no fragment shader, no color attachment. The main color pass can then
// use depth-equal to skip all overdraw.
class DepthPrePass : public RenderPass {
public:
    void create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    /// Create a custom depth pipeline for a specific shader pipeline ID
    /// using a custom vertex shader SPV. Uses the same layout as the default pipeline.
    void register_custom_depth(uint32_t pipeline_id, const std::string& vert_spv_path);

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkPipeline skinned_pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout_{VK_NULL_HANDLE};

    // Per-pipeline-id custom depth pipelines (for vertex displacement)
    std::unordered_map<uint32_t, VkPipeline> custom_depth_pipelines_;

    VkFormat depth_format_{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits sample_count_{VK_SAMPLE_COUNT_1_BIT};
};

} // namespace fjell
