#include "renderer/pass/meshlet_instance_pass.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/vk_check.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace fjell {

void MeshletInstancePass::create(VkDevice device, VkPhysicalDevice physical_device,
                                  VkDescriptorSetLayout global_set_layout) {
    device_ = device;
    physical_device_ = physical_device;

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

        vk_check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci,
                                           nullptr, &expand_pipeline_),
                 "Failed to create meshlet expand compute pipeline");
        vkDestroyShaderModule(device_, module, nullptr);
    }

    // ── Cull pass descriptor layout (set 1) ──
    // binding 0: DrawData[] (read)
    // binding 1: MeshletInstance[] input (read)
    // binding 2: input counter (read)
    // binding 3: MeshletInstance[] visible output (write)
    // binding 4: indirect dispatch cmd / output counter (read-write)
    // binding 5: MeshletBounds[] (read)
    {
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
        for (uint32_t i = 0; i < 6; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        cull_set_layout_ = std::make_unique<DescriptorSetLayout>(device_, std::span{bindings});

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = 6 * MAX_FRAMES_IN_FLIGHT;
        cull_pool_ = std::make_unique<DescriptorPool>(
            device_, std::span{&pool_size, 1}, MAX_FRAMES_IN_FLIGHT);

        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            cull_sets_[i] = cull_pool_->allocate(*cull_set_layout_);
        }
    }

    // Cull pipeline layout: set 0 = global UBO, set 1 = cull buffers
    {
        std::array set_layouts = {global_set_layout, cull_set_layout_->handle()};

        VkPipelineLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
        ci.pSetLayouts = set_layouts.data();

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

        vk_check(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci,
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

    expand_pool_.reset();
    expand_set_layout_.reset();
    cull_pool_.reset();
    cull_set_layout_.reset();
}

void MeshletInstancePass::ensure_capacity(uint32_t frame_index, uint32_t max_instances) {
    if (max_instances <= capacities_[frame_index]) return;

    uint32_t new_cap = std::max(max_instances, capacities_[frame_index] * 2);
    if (new_cap < 256) new_cap = 256;

    VkDeviceSize instance_size = static_cast<VkDeviceSize>(new_cap) * sizeof(GpuMeshletInstance);

    instance_buffers_[frame_index] = std::make_unique<Buffer>(
        device_, physical_device_, instance_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    visible_buffers_[frame_index] = std::make_unique<Buffer>(
        device_, physical_device_, instance_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Atomic counter for expand pass
    expand_counters_[frame_index] = std::make_unique<Buffer>(
        device_, physical_device_, sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Indirect dispatch command: VkDrawMeshTasksIndirectCommandEXT = 3 uint32_t
    indirect_buffers_[frame_index] = std::make_unique<Buffer>(
        device_, physical_device_, 3 * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

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

void MeshletInstancePass::dispatch_cull(VkCommandBuffer cmd,
                                         VkDescriptorSet global_set,
                                         VkBuffer draw_data_buffer,
                                         VkDeviceSize draw_data_size,
                                         VkBuffer bounds_buffer,
                                         VkDeviceSize bounds_size,
                                         uint32_t frame_index) {
    // Zero the indirect dispatch command (groupCountX=0, Y=1, Z=1)
    uint32_t init[3] = {0, 1, 1};
    vkCmdUpdateBuffer(cmd, indirect_buffers_[frame_index]->handle(),
                      0, sizeof(init), init);

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

    // Update cull descriptors
    std::array<VkDescriptorBufferInfo, 6> buffer_infos{};
    buffer_infos[0] = {draw_data_buffer, 0, std::max(draw_data_size, VkDeviceSize{16})};
    buffer_infos[1] = {instance_buffers_[frame_index]->handle(), 0,
                        instance_buffers_[frame_index]->size()};
    buffer_infos[2] = {expand_counters_[frame_index]->handle(), 0, sizeof(uint32_t)};
    buffer_infos[3] = {visible_buffers_[frame_index]->handle(), 0,
                        visible_buffers_[frame_index]->size()};
    buffer_infos[4] = {indirect_buffers_[frame_index]->handle(), 0, 3 * sizeof(uint32_t)};
    buffer_infos[5] = {bounds_buffer, 0, std::max(bounds_size, VkDeviceSize{16})};

    std::array<VkWriteDescriptorSet, 6> writes{};
    for (uint32_t i = 0; i < 6; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = cull_sets_[frame_index];
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(device_, 6, writes.data(), 0, nullptr);

    // The cull shader dispatches based on the expand counter (total meshlet instances).
    // We use vkCmdDispatchIndirect with the expand counter as dispatch X.
    // But the expand counter is just a uint32_t, not a VkDispatchIndirectCommand.
    // Instead, we dispatch with a conservative upper bound from the capacity.
    uint32_t max_groups = (capacities_[frame_index] + 63) / 64;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipeline_);

    std::array desc_sets = {global_set, cull_sets_[frame_index]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cull_layout_,
                            0, static_cast<uint32_t>(desc_sets.size()),
                            desc_sets.data(), 0, nullptr);

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

} // namespace fjell
