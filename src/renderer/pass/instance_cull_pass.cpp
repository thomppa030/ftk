#include "renderer/pass/instance_cull_pass.hpp"

#include "core/base_path.hpp"
#include "core/profiler.hpp"
#include "core/log.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/render_pipeline.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <array>
#include <span>

namespace fjell {

Result<> InstanceCullPass::create(Device& device, const PassResources& resources) {
    device_ = device.handle();

    // Set 1 layout — pass-local buffers:
    //   binding 0: InstancedDraw[] (per-draw AABB + slice offset)
    //   binding 1: InstanceTransforms[] (source, CPU-uploaded)
    //   binding 2: VisibleInstanceTransforms[] (compacted output)
    //   binding 3: IndirectCmds (read-write, instanceCount patched in-place)
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    set_layout_ = std::make_unique<DescriptorSetLayout>(device_, std::span{bindings});

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = static_cast<uint32_t>(bindings.size()) * MAX_FRAMES_IN_FLIGHT;
    pool_ = std::make_unique<DescriptorPool>(device_, std::span{&pool_size, 1}, MAX_FRAMES_IN_FLIGHT);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        descriptor_sets_[i] = pool_->allocate(*set_layout_);
    }

    // Pipeline layout: set 0 = global UBO (view-proj), set 1 = pass buffers.
    std::array set_layouts = {resources.global_set_layout, set_layout_->handle()};
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_ci.pSetLayouts = set_layouts.data();

    vk_check(vkCreatePipelineLayout(device_, &layout_ci, nullptr, &layout_),
             "Failed to create InstanceCullPass pipeline layout");

    auto code = read_spirv(base_path::resolve("shaders/instance_cull.comp.spv"));
    auto module = create_shader_module(device_, code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_ci.stage = stage;
    pipeline_ci.layout = layout_;

    vk_check(vkCreateComputePipelines(device_, resources.pipeline_cache, 1, &pipeline_ci,
                                      nullptr, &pipeline_),
             "Failed to create InstanceCullPass compute pipeline");
    vkDestroyShaderModule(device_, module, nullptr);

    FJELL_GFX_DEBUG("InstanceCullPass created");
    return {};
}

void InstanceCullPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    pool_.reset();
    set_layout_.reset();
}

PassSetup InstanceCullPass::setup() const {
    return {
        .name = "instance_cull",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 10,  // after meshlet_cull (sub_order 0), before depth draw
    };
}

void InstanceCullPass::declare(PassBuilder& builder, const DeclareContext& /*ctx*/) {
    // Writes compacted instance transforms + patched indirect draw
    // commands the vertex path reads through its own descriptor set.
    builder.has_side_effects();
}

void InstanceCullPass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_instance_cull");
#ifdef FJELL_ENABLE_TRACY
    TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "Instance Cull");
#endif
    if (ctx.indirect_batch == nullptr) { return; }
    uint32_t draw_count = ctx.indirect_batch->instanced_draw_count(ctx.frame_index);
    if (draw_count == 0) { return; }

    auto cb = ctx.cmd;
    VkDescriptorSet set = descriptor_sets_[ctx.frame_index];

    std::array<VkDescriptorBufferInfo, 4> buf_infos{};
    buf_infos[0].buffer = ctx.indirect_batch->instanced_draw_buffer(ctx.frame_index);
    buf_infos[0].offset = 0;
    buf_infos[0].range = ctx.indirect_batch->instanced_draw_size(ctx.frame_index);

    buf_infos[1].buffer = ctx.indirect_batch->instance_transform_buffer(ctx.frame_index);
    buf_infos[1].offset = 0;
    buf_infos[1].range = ctx.indirect_batch->instance_transform_size(ctx.frame_index);

    buf_infos[2].buffer = ctx.indirect_batch->visible_instance_buffer(ctx.frame_index);
    buf_infos[2].offset = 0;
    buf_infos[2].range = ctx.indirect_batch->visible_instance_size(ctx.frame_index);

    buf_infos[3].buffer = ctx.indirect_batch->indirect_buffer(ctx.frame_index);
    buf_infos[3].offset = 0;
    buf_infos[3].range = VK_WHOLE_SIZE;

    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &buf_infos[i];
    }
    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    std::array desc_sets = {ctx.global_descriptor_set, set};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, static_cast<uint32_t>(desc_sets.size()),
                            desc_sets.data(), 0, nullptr);

    vkCmdDispatch(cb, draw_count, 1, 1);

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cb, &dep);
}

} // namespace fjell
