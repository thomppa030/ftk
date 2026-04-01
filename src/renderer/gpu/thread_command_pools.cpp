#include "renderer/gpu/thread_command_pools.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <stdexcept>

namespace fjell {

void ThreadCommandPools::create(VkDevice device, uint32_t queue_family, uint32_t thread_count) {
    device_ = device;
    thread_count_ = thread_count;
    pools_.resize(thread_count * MAX_FRAMES_IN_FLIGHT);

    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ci.queueFamilyIndex = queue_family;

    for (auto& ps : pools_) {
        vk_check(vkCreateCommandPool(device_, &ci, nullptr, &ps.pool),
                 "Failed to create thread command pool");
    }
}

void ThreadCommandPools::reset_frame(uint32_t frame_index) {
    for (uint32_t t = 0; t < thread_count_; ++t) {
        auto& ps = pool_at(t, frame_index);
        vkResetCommandPool(device_, ps.pool, 0);
        ps.next = 0;
    }
}

VkCommandBuffer ThreadCommandPools::allocate_secondary(uint32_t thread_index, uint32_t frame_index) {
    auto& ps = pool_at(thread_index, frame_index);

    if (ps.next >= ps.buffers.size()) {
        // Allocate a new batch
        auto old_size = static_cast<uint32_t>(ps.buffers.size());
        ps.buffers.resize(old_size + BATCH_SIZE);

        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = ps.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        ai.commandBufferCount = BATCH_SIZE;

        vk_check(vkAllocateCommandBuffers(device_, &ai, &ps.buffers[old_size]),
                 "Failed to allocate secondary command buffers");
    }

    return ps.buffers[ps.next++];
}

void ThreadCommandPools::destroy() {
    if (!device_) return;
    for (auto& ps : pools_) {
        if (ps.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, ps.pool, nullptr);
            ps.pool = VK_NULL_HANDLE;
        }
        ps.buffers.clear();
        ps.next = 0;
    }
    pools_.clear();
    device_ = VK_NULL_HANDLE;
}

ThreadCommandPools::PoolState& ThreadCommandPools::pool_at(uint32_t thread_index, uint32_t frame_index) {
    return pools_[thread_index * MAX_FRAMES_IN_FLIGHT + frame_index];
}

} // namespace fjell
