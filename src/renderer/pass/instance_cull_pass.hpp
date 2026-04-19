#pragma once

#include "renderer/pass/render_pass.hpp"
#include "renderer/gpu/descriptor.hpp"
#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>

namespace fjell {

// Per-instance frustum culling + compaction for the vertex render path.
// For every GpuInstancedDraw descriptor the pass frustum-tests each instance's
// world AABB, compacts survivors into a visible-transform slice via subgroup
// ballot + prefix sum, and patches the corresponding VkDrawIndexedIndirectCommand
// instanceCount in place. Vertex shaders on the instanced path read from the
// compacted buffer so off-screen instances cost zero vertex work.
//
// Mesh-shader path is unaffected — it culls per meshlet in meshlet_cull.comp.
class InstanceCullPass : public RenderPass {
public:
    [[nodiscard]] Result<> create(Device& device, const PassResources& resources) override;
    void destroy() override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    std::unique_ptr<DescriptorSetLayout> set_layout_;
    std::unique_ptr<DescriptorPool> pool_;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptor_sets_{};
};

} // namespace fjell
