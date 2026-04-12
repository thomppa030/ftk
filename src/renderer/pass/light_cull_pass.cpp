#include "renderer/pass/light_cull_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

namespace fjell {

Result<> LightCullPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();
    vk_pipeline_cache_ = resources.pipeline_cache;

    auto comp_code = read_spirv(base_path::resolve("shaders/light_cull.comp.spv"));
    auto comp_module = create_shader_module(device_, comp_code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = comp_module;
    stage.pName = "main";

    // Push constant: uvec2 viewport_size (8 bytes)
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(uint32_t) * 2;

    VkDescriptorSetLayout global_layout = resources.global_set_layout;
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = 1;
    layout_ci.pSetLayouts = &global_layout;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, comp_module, nullptr);
        return make_error("Failed to create light cull pipeline layout");
    }

    VkComputePipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_ci.stage = stage;
    pipeline_ci.layout = layout_;

    if (vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &pipeline_ci,
                                  nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, comp_module, nullptr);
        return make_error("Failed to create light cull compute pipeline");
    }

    vkDestroyShaderModule(device_, comp_module, nullptr);
    FJELL_GFX_DEBUG("Light cull compute pipeline created");
    return {};
}

void LightCullPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
}

void LightCullPass::record(const FrameContext& ctx) {
    auto extent = ctx.target->extent();

    vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, 1, &ctx.global_descriptor_set, 0, nullptr);

    uint32_t push_data[2] = {extent.width, extent.height};
    vkCmdPushConstants(ctx.cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(push_data), push_data);

    uint32_t groups_x = (extent.width + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    uint32_t groups_y = (extent.height + LIGHT_TILE_SIZE - 1) / LIGHT_TILE_SIZE;
    vkCmdDispatch(ctx.cmd, groups_x, groups_y, 1);
}

PassSetup LightCullPass::setup() const {
    return {
        .name = "light_cull",
        .phase = RenderPhase::GpuDriven,
        .sub_order = 110,
        .reads = {{"depth", ImageUsage::compute_read}},
        .writes = {},
    };
}

void LightCullPass::build(FrameGraph& graph, ResourceRegistry& registry,
                           const FrameContext& ctx) {
    std::vector<std::pair<uint32_t, ImageUsage>> uses;
    if (auto* img = registry.find_image("depth")) {
        uses.emplace_back(img->frame_graph_id, ImageUsage::compute_read);
    }

    graph.add_pass("light_cull", [this, ctx](VkCommandBuffer cb) {
        FrameContext local = ctx;
        local.cmd = cb;
        record(local);

        // Register the tile light SSBO so downstream passes can find it
        if (output_buffer_ != VK_NULL_HANDLE) {
            local.registry->register_buffer("tile_light_ssbo", {
                output_buffer_, output_buffer_size_});
        }

        // Barrier: compute write -> fragment read
        VkMemoryBarrier2 mem_barrier{};
        mem_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mem_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mem_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mem_barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mem_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mem_barrier;
        vkCmdPipelineBarrier2(cb, &dep);
    }, std::move(uses));
}

} // namespace fjell
