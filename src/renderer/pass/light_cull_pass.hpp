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

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
};

} // namespace fjell
