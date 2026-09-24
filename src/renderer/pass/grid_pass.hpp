#pragma once

#include "renderer/pass/render_pass.hpp"

#include <vulkan/vulkan.h>

namespace fjell {

struct GridSettings {
    float cell_size{1.0f};
    float thickness{1.0f};
    float fade_distance{80.0f};
    float opacity{1.0f};
    /// The Scene viewport's grid. Every other viewport keeps its own, on its
    /// render request.
    bool visible{true};

    // Snapping applies to every way an object is moved: the viewport gizmo and
    // the inspector's drag fields. Translation snaps to `cell_size` so the
    // object lands on the grid you can see; rotation and scale have no visual
    // counterpart and carry their own step.
    bool snap_enabled{false};
    float rotation_snap_degrees{15.0f};
    float scale_snap{0.1f};
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
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
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
