#pragma once

#include "renderer/renderer_constants.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

// Per-thread, per-frame command pool manager.
// Vulkan requires external synchronization on command pools, so each
// worker thread needs its own pool.
class ThreadCommandPools {
public:
    void create(VkDevice device, uint32_t queue_family, uint32_t thread_count);
    void reset_frame(uint32_t frame_index);
    [[nodiscard]] VkCommandBuffer allocate_secondary(uint32_t thread_index, uint32_t frame_index);
    void destroy();

private:
    static constexpr uint32_t BATCH_SIZE = 8;

    struct PoolState {
        VkCommandPool pool{VK_NULL_HANDLE};
        std::vector<VkCommandBuffer> buffers;
        uint32_t next{0};
    };

    [[nodiscard]] PoolState& pool_at(uint32_t thread_index, uint32_t frame_index);

    VkDevice device_{VK_NULL_HANDLE};
    uint32_t thread_count_{0};
    // Indexed as [thread_index * MAX_FRAMES_IN_FLIGHT + frame_index]
    std::vector<PoolState> pools_;
};

} // namespace fjell
