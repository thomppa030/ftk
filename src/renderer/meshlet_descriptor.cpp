#include "renderer/meshlet_descriptor.hpp"
#include "renderer/mega_buffer.hpp"
#include "renderer/meshlet_buffer.hpp"
#include "core/log.hpp"

#include <array>

namespace fjell {

void MeshletDescriptor::create(VkDevice device) {
    device_ = device;

    // Set 3 layout: 5 SSBO bindings for meshlet data
    // binding 0: Meshlet[]
    // binding 1: MeshletBounds[]
    // binding 2: uint32_t[] meshlet vertex indices
    // binding 3: uint32_t[] meshlet triangles (packed)
    // binding 4: Vertex[] (mega buffer vertex data)
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};

    for (uint32_t i = 0; i < 5; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    }

    layout_ = std::make_unique<DescriptorSetLayout>(
        device_, std::span{bindings});

    // Pool: 5 SSBOs per frame
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 5 * MAX_FRAMES_IN_FLIGHT;

    pool_ = std::make_unique<DescriptorPool>(
        device_, std::span{&pool_size, 1}, MAX_FRAMES_IN_FLIGHT);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        descriptor_sets_[i] = pool_->allocate(*layout_);
    }

    FJELL_GFX_DEBUG("Meshlet descriptor created (set 3, 5 SSBOs)");
}

void MeshletDescriptor::destroy() {
    pool_.reset();
    layout_.reset();
}

void MeshletDescriptor::update(const MeshletBuffer& meshlet_buf,
                               const MegaBuffer& mega_buf,
                               uint32_t frame_index) {
    std::array<VkDescriptorBufferInfo, 5> buffer_infos{};

    buffer_infos[0].buffer = meshlet_buf.meshlet_buffer();
    buffer_infos[0].offset = 0;
    buffer_infos[0].range = std::max(meshlet_buf.meshlet_buffer_size(), VkDeviceSize{16});

    buffer_infos[1].buffer = meshlet_buf.bounds_buffer();
    buffer_infos[1].offset = 0;
    buffer_infos[1].range = std::max(meshlet_buf.bounds_buffer_size(), VkDeviceSize{16});

    buffer_infos[2].buffer = meshlet_buf.vertex_index_buffer();
    buffer_infos[2].offset = 0;
    buffer_infos[2].range = std::max(meshlet_buf.vertex_index_buffer_size(), VkDeviceSize{16});

    buffer_infos[3].buffer = meshlet_buf.triangle_buffer();
    buffer_infos[3].offset = 0;
    buffer_infos[3].range = std::max(meshlet_buf.triangle_buffer_size(), VkDeviceSize{16});

    buffer_infos[4].buffer = mega_buf.vertex_buffer_handle();
    buffer_infos[4].offset = 0;
    buffer_infos[4].range = std::max(mega_buf.vertex_buffer_size(), VkDeviceSize{16});

    std::array<VkWriteDescriptorSet, 5> writes{};
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_sets_[frame_index];
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &buffer_infos[i];
    }

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

} // namespace fjell
