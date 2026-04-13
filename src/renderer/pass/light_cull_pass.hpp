#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

class LightCullPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void build(FrameGraph& graph, ResourceRegistry& registry,
               const FrameContext& ctx) override;
    void pre_record(const FrameContext& ctx, GpuContext& gpu) override;
    void register_resources(ResourceRegistry& registry) override;

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    VkBuffer output_buffer_{VK_NULL_HANDLE};
    VkDeviceSize output_buffer_size_{0};
};

} // namespace fjell
