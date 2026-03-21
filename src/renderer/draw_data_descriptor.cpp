#include "renderer/draw_data_descriptor.hpp"
#include "core/log.hpp"

#include <array>
#include <stdexcept>

namespace fjell {

void DrawDataDescriptor::create(VkDevice device) {
    device_ = device;

    // Layout: binding 0 = draw data SSBO, binding 1 = bone matrix SSBO
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                             VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_MESH_BIT_EXT |
                             VK_SHADER_STAGE_COMPUTE_BIT;

    layout_ = std::make_unique<DescriptorSetLayout>(
        device_, std::span{bindings});

    // Pool: 2 storage buffer descriptors per frame
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT * 2;

    pool_ = std::make_unique<DescriptorPool>(
        device_, std::span{&pool_size, 1}, MAX_FRAMES_IN_FLIGHT);

    // Allocate per-frame descriptor sets
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        descriptor_sets_[i] = pool_->allocate(*layout_);
    }

    FJELL_GFX_DEBUG("Draw data descriptor created");
}

void DrawDataDescriptor::destroy() {
    pool_.reset();
    layout_.reset();
}

void DrawDataDescriptor::update(VkBuffer buffer, VkDeviceSize size, uint32_t frame_index) {
    if (size == 0) return;

    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = buffer;
    buffer_info.offset = 0;
    buffer_info.range = size;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_sets_[frame_index];
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &buffer_info;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void DrawDataDescriptor::update_bones(VkBuffer buffer, VkDeviceSize size,
                                      uint32_t frame_index) {
    if (size == 0) return;

    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = buffer;
    buffer_info.offset = 0;
    buffer_info.range = size;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_sets_[frame_index];
    write.dstBinding = 1;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &buffer_info;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

} // namespace fjell
