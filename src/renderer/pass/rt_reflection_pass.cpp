#include "renderer/pass/rt_reflection_pass.hpp"

#include "renderer/gpu/device.hpp"
#include "renderer/pass_builder.hpp"

namespace fjell {

// Step 2 scaffold: pass exists, registers, declares no resources, records
// nothing. The RT pipeline + SBT + shaders land in step 3.

Result<> RtReflectionPass::create(Device& device, const PassResources& /*resources*/) {
    device_ = &device;
    vk_device_ = device.handle();
    return {};
}

void RtReflectionPass::destroy() {
    sbt_buffer_.reset();
    if (rt_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(vk_device_, rt_pipeline_, nullptr); rt_pipeline_ = VK_NULL_HANDLE; }
    if (rt_pipeline_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(vk_device_, rt_pipeline_layout_, nullptr); rt_pipeline_layout_ = VK_NULL_HANDLE; }
    if (trace_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(vk_device_, trace_set_layout_, nullptr); trace_set_layout_ = VK_NULL_HANDLE; }
    if (linear_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(vk_device_, linear_sampler_, nullptr); linear_sampler_ = VK_NULL_HANDLE; }
    if (nearest_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(vk_device_, nearest_sampler_, nullptr); nearest_sampler_ = VK_NULL_HANDLE; }
}

PassSetup RtReflectionPass::setup() const {
    return {
        .name = "rt_reflection",
        .phase = RenderPhase::PostProcess,
        // Sit between ssr_trace (0) and ssr_composite (1) so the composite
        // can read whichever output matches the active mode.
        .sub_order = 0,
    };
}

void RtReflectionPass::declare(PassBuilder& /*builder*/, const DeclareContext& /*ctx*/) {
    // Step 3 will declare rt_reflection_output here.
}

void RtReflectionPass::record(const FrameContext& /*ctx*/) {
    // Step 3 will dispatch the RT pipeline here.
}

} // namespace fjell
