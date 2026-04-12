#include "renderer/pass/cull_pass.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/resources/shader_utils.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"

#include <array>
namespace fjell {

Result<> CullPass::create(Device& device, const PassResources& resources) {
    return create(device.handle(), resources.allocator,
                  resources.global_set_layout, resources.pipeline_cache);
}

Result<> CullPass::create(VkDevice device, VmaAllocator allocator,
                          VkDescriptorSetLayout global_set_layout,
                          VkPipelineCache pipeline_cache) {
    device_ = device;
    vk_pipeline_cache_ = pipeline_cache;
    allocator_ = allocator;

    // Set 1 layout: cull-specific buffers
    // binding 0: DrawAABB[] (read-only)
    // binding 1: IndirectCmds (read-write, also used as indirect buffer)
    // binding 2: Visibility[] (write-only)
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    set_layout_ = std::make_unique<DescriptorSetLayout>(device_, std::span{bindings});

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 3 * MAX_FRAMES_IN_FLIGHT;

    pool_ = std::make_unique<DescriptorPool>(
        device_, std::span{&pool_size, 1}, MAX_FRAMES_IN_FLIGHT);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        descriptor_sets_[i] = pool_->allocate(*set_layout_);
    }

    // Pipeline layout: set 0 = global, set 1 = cull buffers
    std::array set_layouts = {global_set_layout, set_layout_->handle()};

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(uint32_t); // total_draws

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_ci.pSetLayouts = set_layouts.data();
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_ci, nullptr, &layout_) != VK_SUCCESS) {
        return make_error("Failed to create cull pass pipeline layout");
    }

    auto comp_code = read_spirv(base_path::resolve("shaders/cull.comp.spv"));
    auto comp_module = create_shader_module(device_, comp_code);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = comp_module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_ci.stage = stage;
    pipeline_ci.layout = layout_;

    if (vkCreateComputePipelines(device_, vk_pipeline_cache_, 1, &pipeline_ci,
                                  nullptr, &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, comp_module, nullptr);
        return make_error("Failed to create cull compute pipeline");
    }

    vkDestroyShaderModule(device_, comp_module, nullptr);
    FJELL_GFX_DEBUG("GPU cull compute pipeline created");
    return {};
}

void CullPass::destroy() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    for (auto& buf : aabb_buffers_) buf.reset();
    for (auto& buf : visibility_buffers_) buf.reset();
    pool_.reset();
    set_layout_.reset();
}

void CullPass::ensure_capacity(uint32_t frame_index, uint32_t count) {
    if (count <= capacities_[frame_index]) return;

    uint32_t new_cap = std::max(count, capacities_[frame_index] * 2);
    if (new_cap < 256) new_cap = 256;

    aabb_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(GpuDrawAABB),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    aabb_buffers_[frame_index]->map();

    visibility_buffers_[frame_index] = std::make_unique<Buffer>(
        allocator_,
        new_cap * sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    visibility_buffers_[frame_index]->map();

    capacities_[frame_index] = new_cap;
}

void CullPass::upload_aabbs(const GpuDrawAABB* data, uint32_t count,
                            uint32_t frame_index) {
    ensure_capacity(frame_index, count);
    aabb_buffers_[frame_index]->write(data, count * sizeof(GpuDrawAABB));
}

void CullPass::dispatch(VkCommandBuffer cmd, VkDescriptorSet global_set,
                        VkBuffer mesh_indirect_buffer, uint32_t total_draws,
                        uint32_t frame_index) {
    if (total_draws == 0) return;

    // Update descriptors for this frame
    std::array<VkDescriptorBufferInfo, 3> buffer_infos{};

    buffer_infos[0].buffer = aabb_buffers_[frame_index]->handle();
    buffer_infos[0].offset = 0;
    buffer_infos[0].range = std::max(VkDeviceSize{16},
        static_cast<VkDeviceSize>(total_draws) * sizeof(GpuDrawAABB));

    buffer_infos[1].buffer = mesh_indirect_buffer;
    buffer_infos[1].offset = 0;
    buffer_infos[1].range = std::max(VkDeviceSize{16},
        static_cast<VkDeviceSize>(total_draws) * sizeof(VkDrawMeshTasksIndirectCommandEXT));

    buffer_infos[2].buffer = visibility_buffers_[frame_index]->handle();
    buffer_infos[2].offset = 0;
    buffer_infos[2].range = std::max(VkDeviceSize{16},
        static_cast<VkDeviceSize>(total_draws) * sizeof(uint32_t));

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_sets_[frame_index];
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &buffer_infos[i];
    }
    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    // Bind and dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);

    std::array desc_sets = {global_set, descriptor_sets_[frame_index]};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_,
                            0, static_cast<uint32_t>(desc_sets.size()),
                            desc_sets.data(), 0, nullptr);

    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(uint32_t), &total_draws);

    uint32_t groups = (total_draws + 63) / 64;
    vkCmdDispatch(cmd, groups, 1, 1);
}

void CullPass::insert_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                           VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT;
    barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace fjell
