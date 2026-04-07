#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

struct GridSettings {
    float cell_size{1.0f};
    float thickness{1.0f};
    float fade_distance{80.0f};
    float opacity{1.0f};
    bool visible{true};
    bool snap_enabled{false};
};

struct GridPushConstants {
    float cell_size;
    float thickness;
    float fade_distance;
    float opacity;
};

class GridPass : public RenderPass {
public:
    Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;

    [[nodiscard]] GridSettings& settings() { return settings_; }
    [[nodiscard]] const GridSettings& settings() const { return settings_; }

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineCache vk_pipeline_cache_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    GridSettings settings_;
};

} // namespace fjell
