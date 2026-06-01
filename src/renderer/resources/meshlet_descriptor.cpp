#include "renderer/resources/meshlet_descriptor.hpp"
#include "renderer/resources/mega_buffer.hpp"
#include "renderer/resources/meshlet_buffer.hpp"
#include "renderer/frame_descriptor_cache.hpp"
#include "core/log.hpp"

#include <array>

namespace fjell {

void MeshletDescriptor::create(VkDevice device, VmaAllocator allocator) {
    device_ = device;
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
                               VkBuffer visible_buffer,
                               VkDeviceSize visible_size) {
    std::array<VkDescriptorBufferInfo, 6> buffer_infos{};

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

    // Binding 5: visible instances (from GPU-driven pipeline, or dummy if unavailable)
    if (visible_buffer != VK_NULL_HANDLE && visible_size > 0) {
        buffer_infos[5].buffer = visible_buffer;
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

    // Cache the shared bindings for acquire_skinned() later in the frame.
    // Skinned draws reuse bindings 0/1/2/3/5 and swap only binding 4 to
    // the per-model SkinnedVertex buffer.
    shared_.meshlets = buffer_infos[0].buffer;
    shared_.meshlets_size = buffer_infos[0].range;
    shared_.bounds = buffer_infos[1].buffer;
    shared_.bounds_size = buffer_infos[1].range;
    shared_.vertex_indices = buffer_infos[2].buffer;
    shared_.vertex_indices_size = buffer_infos[2].range;
    shared_.triangles = buffer_infos[3].buffer;
    shared_.triangles_size = buffer_infos[3].range;
    shared_.visible = buffer_infos[5].buffer;
    shared_.visible_size = buffer_infos[5].range;
}

VkDescriptorSet MeshletDescriptor::acquire_skinned(
    FrameDescriptorCache& cache,
    VkBuffer skinned_vertex_buffer,
    VkDeviceSize skinned_vertex_buffer_size) const {
    if (skinned_vertex_buffer == VK_NULL_HANDLE || skinned_vertex_buffer_size == 0) {
        return VK_NULL_HANDLE;
    }

    std::array<FrameCacheBinding, 6> bindings{};
    bindings[0].binding = 0;
    bindings[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].buffer = {shared_.meshlets, 0, shared_.meshlets_size};

    bindings[1].binding = 1;
    bindings[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].buffer = {shared_.bounds, 0, shared_.bounds_size};

    bindings[2].binding = 2;
    bindings[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].buffer = {shared_.vertex_indices, 0, shared_.vertex_indices_size};

    bindings[3].binding = 3;
    bindings[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].buffer = {shared_.triangles, 0, shared_.triangles_size};

    bindings[4].binding = 4;
    bindings[4].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].buffer = {skinned_vertex_buffer, 0, skinned_vertex_buffer_size};

    bindings[5].binding = 5;
    bindings[5].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].buffer = {shared_.visible, 0, shared_.visible_size};

    return cache.acquire(layout_->handle(), std::span{bindings});
}

} // namespace fjell
