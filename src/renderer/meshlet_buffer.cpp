#include "renderer/meshlet_buffer.hpp"
#include "core/log.hpp"

#include <cstring>
#include <stdexcept>

namespace fjell {

MeshletBuffer::MeshletBuffer(VkDevice device, VkPhysicalDevice physical_device,
                             VkCommandPool command_pool, VkQueue queue)
    : device_{device}
    , physical_device_{physical_device}
    , command_pool_{command_pool}
    , queue_{queue}
    , meshlet_capacity_{INITIAL_MESHLET_SIZE}
    , bounds_capacity_{INITIAL_BOUNDS_SIZE}
    , vertex_index_capacity_{INITIAL_VERTEX_INDEX_SIZE}
    , triangle_capacity_{INITIAL_TRIANGLE_SIZE} {
    constexpr VkBufferUsageFlags ssbo_usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    meshlet_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, meshlet_capacity_, ssbo_usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    bounds_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, bounds_capacity_, ssbo_usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vertex_index_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, vertex_index_capacity_, ssbo_usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    triangle_buffer_ = std::make_unique<Buffer>(
        device_, physical_device_, triangle_capacity_, ssbo_usage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    FJELL_GFX_INFO("MeshletBuffer created: {}KB meshlets, {}KB bounds, {}KB vert_idx, {}KB tris",
                   meshlet_capacity_ / 1024, bounds_capacity_ / 1024,
                   vertex_index_capacity_ / 1024, triangle_capacity_ / 1024);
}

MeshletRange MeshletBuffer::upload(const MeshletBuildResult& result,
                                   uint32_t mega_vertex_offset) {
    if (result.meshlets.empty()) return {};

    VkDeviceSize meshlet_size = result.meshlets.size() * sizeof(Meshlet);
    VkDeviceSize bounds_size = result.bounds.size() * sizeof(MeshletBounds);
    VkDeviceSize vert_idx_size = result.meshlet_vertices.size() * sizeof(uint32_t);

    // Pack uint8 triangles into uint32 for GPU access (pad to 4 bytes)
    VkDeviceSize tri_size_raw = result.meshlet_triangles.size();
    VkDeviceSize tri_size_padded = (tri_size_raw + 3) & ~3;

    // Grow if needed
    if (meshlet_used_ + meshlet_size > meshlet_capacity_) {
        grow_buffer(meshlet_buffer_, (meshlet_used_ + meshlet_size) * 2,
                    meshlet_used_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        meshlet_capacity_ = (meshlet_used_ + meshlet_size) * 2;
    }
    if (bounds_used_ + bounds_size > bounds_capacity_) {
        grow_buffer(bounds_buffer_, (bounds_used_ + bounds_size) * 2,
                    bounds_used_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        bounds_capacity_ = (bounds_used_ + bounds_size) * 2;
    }
    if (vertex_index_used_ + vert_idx_size > vertex_index_capacity_) {
        grow_buffer(vertex_index_buffer_, (vertex_index_used_ + vert_idx_size) * 2,
                    vertex_index_used_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vertex_index_capacity_ = (vertex_index_used_ + vert_idx_size) * 2;
    }
    if (triangle_used_ + tri_size_padded > triangle_capacity_) {
        grow_buffer(triangle_buffer_, (triangle_used_ + tri_size_padded) * 2,
                    triangle_used_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        triangle_capacity_ = (triangle_used_ + tri_size_padded) * 2;
    }

    // Stage all data in one command buffer
    Buffer meshlet_staging{device_, physical_device_, meshlet_size,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    meshlet_staging.write(result.meshlets.data(), meshlet_size);

    Buffer bounds_staging{device_, physical_device_, bounds_size,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    bounds_staging.write(result.bounds.data(), bounds_size);

    Buffer vert_staging{device_, physical_device_, vert_idx_size,
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    vert_staging.write(result.meshlet_vertices.data(), vert_idx_size);

    // Pad triangle data to 4-byte alignment
    std::vector<uint8_t> padded_tris(tri_size_padded, 0);
    std::memcpy(padded_tris.data(), result.meshlet_triangles.data(), tri_size_raw);
    Buffer tri_staging{device_, physical_device_, tri_size_padded,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    tri_staging.write(padded_tris.data(), tri_size_padded);

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool = command_pool_;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    auto copy = [&](Buffer& src, Buffer& dst, VkDeviceSize dst_offset, VkDeviceSize size) {
        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = dst_offset;
        region.size = size;
        vkCmdCopyBuffer(cmd, src.handle(), dst.handle(), 1, &region);
    };

    copy(meshlet_staging, *meshlet_buffer_, meshlet_used_, meshlet_size);
    copy(bounds_staging, *bounds_buffer_, bounds_used_, bounds_size);
    copy(vert_staging, *vertex_index_buffer_, vertex_index_used_, vert_idx_size);
    copy(tri_staging, *triangle_buffer_, triangle_used_, tri_size_padded);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);

    MeshletRange range{};
    range.meshlet_offset = meshlet_count_;
    range.meshlet_count = static_cast<uint32_t>(result.meshlets.size());
    range.vertex_offset = mega_vertex_offset;

    meshlet_count_ += range.meshlet_count;
    meshlet_used_ += meshlet_size;
    bounds_used_ += bounds_size;
    vertex_index_used_ += vert_idx_size;
    triangle_used_ += tri_size_padded;

    return range;
}

void MeshletBuffer::grow_buffer(std::unique_ptr<Buffer>& buf, VkDeviceSize new_size,
                                VkDeviceSize used, VkBufferUsageFlags usage) {
    FJELL_GFX_INFO("MeshletBuffer: growing buffer to {}KB", new_size / 1024);

    auto new_buf = std::make_unique<Buffer>(
        device_, physical_device_, new_size,
        usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (used > 0) {
        Buffer::copy(device_, command_pool_, queue_, *buf, *new_buf, used);
    }

    buf = std::move(new_buf);
}

} // namespace fjell
