#include "renderer/draw_data_descriptor.hpp"
#include "core/log.hpp"

#include <array>
#include <stdexcept>

namespace fjell {

void DrawDataDescriptor::create(VkDevice device) {
    device_ = device;

    // Layout: binding 0 = storage buffer (draw data SSBO)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;

    layout_ = std::make_unique<DescriptorSetLayout>(
        device_, std::span{&binding, 1});

    // Pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = MAX_FRAMES_IN_FLIGHT;

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

} // namespace fjell
