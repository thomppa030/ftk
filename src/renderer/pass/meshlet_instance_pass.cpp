#include "renderer/pass/meshlet_instance_pass.hpp"
#include "core/profiler.hpp"
#include "renderer/pass/hiz_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/indirect_batch.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/pass_builder.hpp"
#include "renderer/pass_registry.hpp"
#include "renderer/post_process_resources.hpp"
#include "renderer/render_stats.hpp"
#include "renderer/resource_registry.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#ifdef FJELL_ENABLE_TRACY
#include <tracy/TracyVulkan.hpp>
#endif

#include <algorithm>
#include <array>
#include <stdexcept>

namespace fjell {

Result<> MeshletInstancePass::create(Device& device, const PassResources& resources) {
    draw_data_set_layout_ = resources.draw_data_layout;
    create(device.handle(), resources.allocator,
           resources.global_set_layout, resources.pipeline_cache);
    return {};
}

void MeshletInstancePass::create(VkDevice device, VmaAllocator allocator,
                                  VkDescriptorSetLayout global_set_layout,
                                  VkPipelineCache pipeline_cache) {
    device_ = device;
    vk_pipeline_cache_ = pipeline_cache;
    allocator_ = allocator;

    // ── Expand pass descriptor layout (set 0) ──
    // binding 0: DrawData[] (read-only)
    // binding 1: MeshletInstance[] output (write-only)
    // binding 2: atomic counter (read-write)
    {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        expand_set_layout_ = std::make_unique<DescriptorSetLayout>(device_, std::span{bindings});

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = 3 * MAX_FRAMES_IN_FLIGHT;
        expand_pool_ = std::make_unique<DescriptorPool>(
            device_, std::span{&pool_size, 1}, MAX_FRAMES_IN_FLIGHT);

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            expand_sets_[i] = expand_pool_->allocate(*expand_set_layout_);
        }
    }

    // Expand pipeline layout: single set + push constant
    {
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(uint32_t);

        VkDescriptorSetLayout set = expand_set_layout_->handle();
        VkPipelineLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &set;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &push_range;

        vk_check(vkCreatePipelineLayout(device_, &ci, nullptr, &expand_layout_),
                 "Failed to create meshlet expand pipeline layout");
    }

    // Expand compute pipeline
    {
        auto code = read_spirv(base_path::resolve("shaders/meshlet_expand.comp.spv"));
        auto module = create_shader_module(device_, code);

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";

        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = stage;
        ci.layout = expand_layout_;

        vk_check(vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &ci,
                                           nullptr, &expand_pipeline_),
                 "Failed to create meshlet expand compute pipeline");
        vkDestroyShaderModule(device_, module, nullptr);
    }

    // ── Cull pass descriptor layout (set 1) ──
    // binding 0: DrawData[] (read)
    // binding 1: MeshletInstance[] input (read)
    // binding 2: input counter (read)
    // binding 3: MeshletInstance[] visible output (write)
    // binding 4: indirect dispatch cmds / per-bin counters (read-write)
    // binding 5: MeshletBounds[] (read)
    // binding 6: uint pipeline_bin[] (read) — per-draw bin index
    // binding 7: Hi-Z pyramid sampler (read) — for occlusion culling
    // binding 8: MeshletInstance[] culled output (write in pass 1, read in pass 2)
    // binding 9: culled counter (read-write)
    {
        std::array<VkDescriptorSetLayoutBinding, 10> bindings{};
        for (uint32_t i = 0; i < 10; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

        // Allow binding 7 to be unbound when not accessed (shader checks hiz_mip_count)
        std::array<VkDescriptorBindingFlags, 10> binding_flags{};
        binding_flags[7] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;

        VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
        flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flags_ci.bindingCount = static_cast<uint32_t>(binding_flags.size());
        flags_ci.pBindingFlags = binding_flags.data();

        cull_set_layout_ = std::make_unique<DescriptorSetLayout>(
            device_, std::span{bindings}, &flags_ci);

        // 2x sets: separate sets for pass 1 and pass 2 per frame
        constexpr uint32_t SETS_PER_FRAME = 2;
        std::array<VkDescriptorPoolSize, 2> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9 * MAX_FRAMES_IN_FLIGHT * SETS_PER_FRAME};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT * SETS_PER_FRAME};

        cull_pool_ = std::make_unique<DescriptorPool>(
            device_, std::span{pool_sizes}, MAX_FRAMES_IN_FLIGHT * SETS_PER_FRAME);

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            cull_sets_[i] = cull_pool_->allocate(*cull_set_layout_);
            cull_sets_pass2_[i] = cull_pool_->allocate(*cull_set_layout_);
        }
    }

    // Cull pipeline layout: set 0 = global UBO, set 1 = cull buffers,
    // set 2 = DrawData + InstanceTransforms (shared with vertex/mesh shaders)
    {
        std::array set_layouts = {
            global_set_layout,
            cull_set_layout_->handle(),
            draw_data_set_layout_,
        };

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = 5 * sizeof(uint32_t); // segment_size, vp_width, vp_height, hiz_mips, is_second_pass

        VkPipelineLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
        ci.pSetLayouts = set_layouts.data();
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &push_range;

        vk_check(vkCreatePipelineLayout(device_, &ci, nullptr, &cull_layout_),
                 "Failed to create meshlet cull pipeline layout");
    }

    // Cull compute pipeline
    {
        auto code = read_spirv(base_path::resolve("shaders/meshlet_cull.comp.spv"));
        auto module = create_shader_module(device_, code);

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";

        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = stage;
        ci.layout = cull_layout_;

        vk_check(vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &ci,
                                           nullptr, &cull_pipeline_),
                 "Failed to create meshlet cull compute pipeline");
        vkDestroyShaderModule(device_, module, nullptr);
    }

    FJELL_GFX_DEBUG("MeshletInstancePass created (expand + cull pipelines)");
}

void MeshletInstancePass::destroy() {
    if (expand_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, expand_pipeline_, nullptr);
    if (expand_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, expand_layout_, nullptr);
    if (cull_pipeline_ != VK_NULL_HANDLE)
        vkDestroyPipeline(device_, cull_pipeline_, nullptr);
    if (cull_layout_ != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(device_, cull_layout_, nullptr);

    for (auto& buf : instance_buffers_) buf.reset();
    for (auto& buf : expand_counters_) buf.reset();
    for (auto& buf : visible_buffers_) buf.reset();
    for (auto& buf : indirect_buffers_) buf.reset();
    for (auto& buf : culled_buffers_) buf.reset();
    for (auto& buf : culled_counters_) buf.reset();

    expand_pool_.reset();
    expand_set_layout_.reset();
    cull_pool_.reset();
    cull_set_layout_.reset();
}

void MeshletInstancePass::pre_record(const FrameContext& ctx, GpuContext& /*gpu*/) {
    if (!ctx.indirect_batch) return;
    uint32_t total_mi = ctx.indirect_batch->total_meshlet_instances(ctx.frame_index);
    if (total_mi > 0) {
        ensure_capacity(ctx.frame_index, total_mi);
    }
}

void MeshletInstancePass::ensure_capacity(uint32_t frame_index, uint32_t max_instances) {
    if (max_instances <= capacities_[frame_index]) return;

    uint32_t new_cap = std::max(max_instances, capacities_[frame_index] * 2);
    if (new_cap < 256) new_cap = 256;

    VkDeviceSize instance_size = static_cast<VkDeviceSize>(new_cap) * sizeof(GpuMeshletInstance);

    instance_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_, instance_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Visible buffer: N segments, each capable of holding all instances
    VkDeviceSize visible_size = instance_size * MAX_PIPELINE_BINS;
    visible_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_, visible_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Atomic counter for expand pass
    expand_counters_[frame_index] = std::make_unique<Buffer>(
        allocator_, sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Indirect dispatch commands: one VkDrawMeshTasksIndirectCommandEXT per bin
    VkDeviceSize indirect_size = MAX_PIPELINE_BINS * sizeof(VkDrawMeshTasksIndirectCommandEXT);
    indirect_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_, indirect_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Culled instances buffer (for two-pass occlusion: stores Hi-Z rejected from first pass)
    culled_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_, instance_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    // Culled counter
    culled_counters_[frame_index] = std::make_unique<Buffer>(
        allocator_, sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO);

    capacities_[frame_index] = new_cap;
}

void MeshletInstancePass::dispatch_expand(VkCommandBuffer cmd,
                                           VkBuffer draw_data_buffer,
                                           VkDeviceSize draw_data_size,
                                           uint32_t total_draws,
                                           uint32_t frame_index) {
    if (total_draws == 0) return;

    // Zero the atomic counter
    vkCmdFillBuffer(cmd, expand_counters_[frame_index]->handle(),
                    0, sizeof(uint32_t), 0);

    // Barrier: transfer write → compute read/write
    VkMemoryBarrier2 fill_barrier{};
    fill_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    fill_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    fill_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    fill_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    fill_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &fill_barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Update expand descriptors
    std::array<VkDescriptorBufferInfo, 3> buffer_infos{};
    buffer_infos[0] = {draw_data_buffer, 0, std::max(draw_data_size, VkDeviceSize{16})};
    buffer_infos[1] = {instance_buffers_[frame_index]->handle(), 0,
                        instance_buffers_[frame_index]->size()};
    buffer_infos[2] = {expand_counters_[frame_index]->handle(), 0, sizeof(uint32_t)};

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = expand_sets_[frame_index];
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(device_, 3, writes.data(), 0, nullptr);

    // Bind and dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, expand_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, expand_layout_,
                            0, 1, &expand_sets_[frame_index], 0, nullptr);
    vkCmdPushConstants(cmd, expand_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(uint32_t), &total_draws);

    uint32_t groups = (total_draws + 63) / 64;
    vkCmdDispatch(cmd, groups, 1, 1);
}

void MeshletInstancePass::insert_expand_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void MeshletInstancePass::dispatch_cull_first(VkCommandBuffer cmd,
                                               VkDescriptorSet global_set,
                                               VkDescriptorSet draw_data_set,
                                               VkBuffer draw_data_buffer,
                                               VkDeviceSize draw_data_size,
                                               VkBuffer bounds_buffer,
                                               VkDeviceSize bounds_size,
                                               VkBuffer pipeline_bin_buffer,
                                               VkDeviceSize pipeline_bin_size,
                                               uint32_t num_bins,
                                               uint32_t frame_index,
                                               VkImageView hiz_view,
                                               VkSampler hiz_sampler,
                                               uint32_t viewport_width,
                                               uint32_t viewport_height,
                                               uint32_t hiz_mip_count) {
    // Zero the culled counter for first pass
    vkCmdFillBuffer(cmd, culled_counters_[frame_index]->handle(),
                    0, sizeof(uint32_t), 0);

    dispatch_cull_internal(cmd, global_set, draw_data_set, draw_data_buffer, draw_data_size,
                           bounds_buffer, bounds_size, pipeline_bin_buffer, pipeline_bin_size,
                           num_bins, frame_index, hiz_view, hiz_sampler,
                           viewport_width, viewport_height, hiz_mip_count, 0);
}

void MeshletInstancePass::dispatch_cull_second(VkCommandBuffer cmd,
                                                VkDescriptorSet global_set,
                                                VkDescriptorSet draw_data_set,
                                                VkBuffer draw_data_buffer,
                                                VkDeviceSize draw_data_size,
                                                VkBuffer bounds_buffer,
                                                VkDeviceSize bounds_size,
                                                VkBuffer pipeline_bin_buffer,
                                                VkDeviceSize pipeline_bin_size,
                                                uint32_t num_bins,
                                                uint32_t frame_index,
                                                VkImageView hiz_view,
                                                VkSampler hiz_sampler,
                                                uint32_t viewport_width,
                                                uint32_t viewport_height,
                                                uint32_t hiz_mip_count) {
    // Second pass: the culled buffer from first pass becomes the input.
    // Don't re-zero indirect commands — we APPEND to the existing visible buffer.
    dispatch_cull_internal(cmd, global_set, draw_data_set, draw_data_buffer, draw_data_size,
                           bounds_buffer, bounds_size, pipeline_bin_buffer, pipeline_bin_size,
                           num_bins, frame_index, hiz_view, hiz_sampler,
                           viewport_width, viewport_height, hiz_mip_count, 1);
}

void MeshletInstancePass::dispatch_cull_internal(VkCommandBuffer cmd,
                                                  VkDescriptorSet global_set,
                                                  VkDescriptorSet draw_data_set,
                                                  VkBuffer draw_data_buffer,
                                                  VkDeviceSize draw_data_size,
                                                  VkBuffer bounds_buffer,
                                                  VkDeviceSize bounds_size,
                                                  VkBuffer pipeline_bin_buffer,
                                                  VkDeviceSize pipeline_bin_size,
                                                  uint32_t num_bins,
                                                  uint32_t frame_index,
                                                  VkImageView hiz_view,
                                                  VkSampler hiz_sampler,
                                                  uint32_t viewport_width,
                                                  uint32_t viewport_height,
                                                  uint32_t hiz_mip_count,
                                                  uint32_t is_second_pass) {
    // For first pass: zero all N indirect dispatch commands
    // For second pass: skip zeroing (append to existing results)
    if (is_second_pass == 0) {
        uint32_t cmd_count = std::max(num_bins, 1u);
        VkDeviceSize init_size = cmd_count * sizeof(VkDrawMeshTasksIndirectCommandEXT);
        vkCmdFillBuffer(cmd, indirect_buffers_[frame_index]->handle(),
                        0, init_size, 0);

        // Write Y=1, Z=1 for each command
        for (uint32_t i = 0; i < cmd_count; ++i) {
            uint32_t one = 1;
            VkDeviceSize y_offset = i * sizeof(VkDrawMeshTasksIndirectCommandEXT) + sizeof(uint32_t);
            VkDeviceSize z_offset = y_offset + sizeof(uint32_t);
            vkCmdUpdateBuffer(cmd, indirect_buffers_[frame_index]->handle(),
                              y_offset, sizeof(uint32_t), &one);
            vkCmdUpdateBuffer(cmd, indirect_buffers_[frame_index]->handle(),
                              z_offset, sizeof(uint32_t), &one);
        }
    }

    // Barrier: transfer → compute
    VkMemoryBarrier2 fill_barrier{};
    fill_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    fill_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    fill_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    fill_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    fill_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &fill_barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    // For second pass, the input buffer is the culled buffer from first pass
    VkBuffer input_buffer = is_second_pass == 0
        ? instance_buffers_[frame_index]->handle()
        : culled_buffers_[frame_index]->handle();
    VkDeviceSize input_size = is_second_pass == 0
        ? instance_buffers_[frame_index]->size()
        : culled_buffers_[frame_index]->size();
    VkBuffer input_counter = is_second_pass == 0
        ? expand_counters_[frame_index]->handle()
        : culled_counters_[frame_index]->handle();

    // Update cull descriptors (9 buffer bindings + 1 image binding)
    std::array<VkDescriptorBufferInfo, 10> buffer_infos{};
    buffer_infos[0] = {draw_data_buffer, 0, std::max(draw_data_size, VkDeviceSize{16})};
    buffer_infos[1] = {input_buffer, 0, input_size};
    buffer_infos[2] = {input_counter, 0, sizeof(uint32_t)};
    buffer_infos[3] = {visible_buffers_[frame_index]->handle(), 0,
                        visible_buffers_[frame_index]->size()};
    buffer_infos[4] = {indirect_buffers_[frame_index]->handle(), 0,
                        indirect_buffers_[frame_index]->size()};
    buffer_infos[5] = {bounds_buffer, 0, std::max(bounds_size, VkDeviceSize{16})};
    buffer_infos[6] = {pipeline_bin_buffer, 0, std::max(pipeline_bin_size, VkDeviceSize{16})};
    // binding 7 is image, handled below
    buffer_infos[8] = {culled_buffers_[frame_index]->handle(), 0,
                        culled_buffers_[frame_index]->size()};
    buffer_infos[9] = {culled_counters_[frame_index]->handle(), 0, sizeof(uint32_t)};

    VkDescriptorSet active_set = is_second_pass == 0
        ? cull_sets_[frame_index]
        : cull_sets_pass2_[frame_index];

    std::array<VkWriteDescriptorSet, 10> writes{};
    for (uint32_t i = 0; i < 10; ++i) {
        if (i == 7) continue; // image binding, handled separately
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = active_set;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &buffer_infos[i];
    }

    // Binding 7: Hi-Z pyramid sampler (partially bound — only write when available)
    VkDescriptorImageInfo hiz_info{};
    if (hiz_view != VK_NULL_HANDLE) {
        hiz_info.sampler = hiz_sampler;
        hiz_info.imageView = hiz_view;
        hiz_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet = active_set;
        writes[7].dstBinding = 7;
        writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[7].descriptorCount = 1;
        writes[7].pImageInfo = &hiz_info;
    }

    // Build a contiguous array of valid writes (skip binding 7 if no Hi-Z)
    std::array<VkWriteDescriptorSet, 10> valid_writes{};
    uint32_t valid_count = 0;
    for (uint32_t i = 0; i < 10; ++i) {
        if (i == 7 && hiz_view == VK_NULL_HANDLE) continue;
        if (writes[i].sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET) {
            valid_writes[valid_count++] = writes[i];
        }
    }

    vkUpdateDescriptorSets(device_, valid_count, valid_writes.data(), 0, nullptr);

    uint32_t seg_size = capacities_[frame_index];
    uint32_t max_groups = (seg_size + 63) / 64;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline_);

    std::array desc_sets = {global_set, active_set, draw_data_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_layout_,
                            0, static_cast<uint32_t>(desc_sets.size()),
                            desc_sets.data(), 0, nullptr);

    uint32_t push[5] = {seg_size, viewport_width, viewport_height, hiz_mip_count, is_second_pass};
    vkCmdPushConstants(cmd, cull_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(push), push);

    vkCmdDispatch(cmd, max_groups, 1, 1);
}

void MeshletInstancePass::insert_cull_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                            VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT |
                            VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                             VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

PassSetup MeshletInstancePass::setup() const {
    return {
        .name = "meshlet_cull",
        .phase = RenderPhase::DepthPrepass,
        .sub_order = 0,
    };
}

void MeshletInstancePass::declare(PassBuilder& builder, const DeclareContext& ctx) {
    auto it = ctx.imports.find(fg_name_hash("hiz_pyramid"));
    if (it != ctx.imports.end()) {
        auto hiz = builder.import_named(ctx, "hiz_pyramid");
        builder.read(hiz, ResourceAccess::sampled_compute);
    }
    // Writes the visible/indirect/culled SSBO chain consumed by the
    // mesh-shader depth/color passes through the meshlet descriptor
    // set — those accesses are outside the DAG.
    builder.has_side_effects();
}

void MeshletInstancePass::record(const FrameContext& ctx) {
    FJELL_PROFILE_SCOPE_N("record_meshlet_instance");
#ifdef FJELL_ENABLE_TRACY
    TracyVkZone(static_cast<tracy::VkCtx*>(ctx.tracy_ctx), ctx.cmd, "Meshlet Expand+Cull");
#endif
    if (ctx.indirect_batch == nullptr) { return; }

    uint32_t total_draws = ctx.indirect_batch->opaque_draw_count(ctx.frame_index);
    uint32_t total_mi = ctx.indirect_batch->total_meshlet_instances(ctx.frame_index);
    if (total_draws == 0 || total_mi == 0) { return; }

    auto cb = ctx.cmd;

    dispatch_expand(
        cb,
        ctx.indirect_batch->draw_data_buffer(ctx.frame_index),
        ctx.indirect_batch->draw_data_size(ctx.frame_index),
        total_draws, ctx.frame_index);
    insert_expand_barrier(cb);

    auto& ranges_ref = ctx.indirect_batch->pipeline_ranges(ctx.frame_index);
    auto extent = ctx.target->extent();

    VkBuffer bounds_buf = VK_NULL_HANDLE;
    VkDeviceSize bounds_size = 0;
    if (ctx.registry != nullptr) {
        if (auto* bounds = ctx.registry->find_buffer("meshlet_bounds")) {
            bounds_buf = bounds->buffer;
            bounds_size = bounds->size;
        }
    }

    auto* hiz_imgs = ctx.pp_resources ? ctx.pp_resources->hiz() : nullptr;
    bool hiz_ready = hiz_imgs != nullptr && hiz_imgs->ready();

    VkSampler hiz_sampler = VK_NULL_HANDLE;
    if (hiz_ready && ctx.passes != nullptr) {
        if (auto* hiz = ctx.passes->try_get<HizPass>("hiz")) {
            hiz_sampler = hiz->sampler();
        }
    }

    dispatch_cull_first(
        cb,
        ctx.global_descriptor_set,
        ctx.draw_data_set,
        ctx.indirect_batch->draw_data_buffer(ctx.frame_index),
        ctx.indirect_batch->draw_data_size(ctx.frame_index),
        bounds_buf, bounds_size,
        ctx.indirect_batch->pipeline_bin_buffer(ctx.frame_index),
        ctx.indirect_batch->pipeline_bin_size(ctx.frame_index),
        static_cast<uint32_t>(ranges_ref.size()),
        ctx.frame_index,
        hiz_ready ? hiz_imgs->hiz_sampled_view : VK_NULL_HANDLE,
        hiz_sampler,
        extent.width, extent.height,
        hiz_ready ? hiz_imgs->hiz_mip_count : 0);

    insert_cull_barrier(cb);
    if (ctx.render_stats != nullptr) {
        ctx.render_stats->two_pass_occlusion = hiz_ready;
    }
}

} // namespace fjell
