#include "renderer/resources/meshlet_descriptor.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "gpu/vulkan/frame_descriptor_cache.hpp"
#include "gpu/vulkan/native.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <array>

namespace fjell {

void MeshletDescriptor::create(VkDevice device, gpu::Device& gpu, VmaAllocator allocator) {
    device_ = device;
    gpu_ = &gpu;
    allocator_ = allocator;

    // Set 3 layout: 6 SSBO bindings for meshlet data
    // binding 0: Meshlet[]
    // binding 1: MeshletBounds[]
    // binding 2: uint32_t[] meshlet vertex indices
    // binding 3: uint32_t[] meshlet triangles (packed)
    // binding 4: Vertex[] (mega buffer vertex data)
    // binding 5: MeshletInstance[] visible instances (GPU-driven pipeline)
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};

    for (uint32_t i = 0; i < 6; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    }

    layout_ = std::make_unique<DescriptorSetLayout>(
        device_, std::span{bindings});
    layout_bindings_ = bindings;

    // Pool: 6 SSBOs per frame
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 6 * MAX_FRAMES_IN_FLIGHT;

    pool_ = std::make_unique<DescriptorPool>(
        device_, std::span{&pool_size, 1}, MAX_FRAMES_IN_FLIGHT);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        descriptor_sets_[i] = pool_->allocate(*layout_);
    }

    // Create a small dummy buffer for binding 5 when no visible buffer is available
    dummy_buffer_ = std::make_unique<Buffer>(
        allocator_, 16,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO);

    FJELL_GFX_DEBUG("Meshlet descriptor created (set 3, 6 SSBOs)");
}

void MeshletDescriptor::destroy() {
    dummy_buffer_.reset();
    pool_.reset();
    layout_.reset();
}

void MeshletDescriptor::update(const MeshletBuffer& meshlet_buf,
                               const MegaBuffer& mega_buf,
                               uint32_t frame_index,
                               gpu::Buffer visible_buffer,
                               uint64_t visible_size) {
    std::array<VkDescriptorBufferInfo, 6> buffer_infos{};
    // Each bound up to what is uploaded, and never empty.
    const auto whole = [&](gpu::Buffer buffer, uint64_t size) {
        return VkDescriptorBufferInfo{gpu::vulkan::native_buffer(*gpu_, buffer), 0,
                                      std::max<VkDeviceSize>(size, 16)};
    };
    buffer_infos[0] = whole(meshlet_buf.meshlet_buffer(), meshlet_buf.meshlet_buffer_size());
    buffer_infos[1] = whole(meshlet_buf.bounds_buffer(), meshlet_buf.bounds_buffer_size());
    buffer_infos[2] = whole(meshlet_buf.vertex_index_buffer(), meshlet_buf.vertex_index_buffer_size());
    buffer_infos[3] = whole(meshlet_buf.triangle_buffer(), meshlet_buf.triangle_buffer_size());
    buffer_infos[4] = whole(mega_buf.vertex_buffer(), mega_buf.vertex_buffer_size());

    // Binding 5: visible instances (from GPU-driven pipeline, or dummy if unavailable)
    if (visible_buffer.valid() && visible_size > 0) {
        buffer_infos[5].buffer = gpu::vulkan::native_buffer(*gpu_, visible_buffer);
        buffer_infos[5].offset = 0;
        buffer_infos[5].range = visible_size;
    } else {
        buffer_infos[5].buffer = dummy_buffer_->handle();
        buffer_infos[5].offset = 0;
        buffer_infos[5].range = 16;
    }

    std::array<VkWriteDescriptorSet, 6> writes{};
    for (uint32_t i = 0; i < 6; ++i) {
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
