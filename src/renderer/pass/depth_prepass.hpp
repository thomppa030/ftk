#pragma once

#include "renderer/pass/depth_pipeline.hpp"
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
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    [[nodiscard]] bool uses_declare() const noexcept override { return true; }

    void register_custom_depth(uint32_t pipeline_id, const std::string& vert_spv_path);

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkPipeline skinned_pipeline_{VK_NULL_HANDLE};
    VkDescriptorSetLayout draw_data_layout_{VK_NULL_HANDLE};

    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    std::unordered_map<uint32_t, VkPipeline> custom_depth_pipelines_;
    DepthPipelineConfig depth_config_;
};

} // namespace fjell
