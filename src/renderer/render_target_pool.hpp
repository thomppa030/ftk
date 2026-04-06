#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

/// Description of a transient render target.
struct RenderTargetDesc {
    uint32_t width{0};
    uint32_t height{0};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    VkImageUsageFlags usage{VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
};

/// A pooled render target — image + view + allocation.
struct RenderTarget {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VmaAllocation allocation{VK_NULL_HANDLE};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
};

/// Pool of transient VkImage render targets, reused across passes each frame.
/// Acquire a target matching a description; release it when done. Targets are
/// recycled within the same frame or across frames. Avoids per-pass manual
/// create_images/destroy_images boilerplate for intermediate targets.
class RenderTargetPool {
public:
    RenderTargetPool(VkDevice device, VmaAllocator allocator);
    ~RenderTargetPool();

    RenderTargetPool(const RenderTargetPool&) = delete;
    RenderTargetPool& operator=(const RenderTargetPool&) = delete;
    RenderTargetPool(RenderTargetPool&&) = delete;
    RenderTargetPool& operator=(RenderTargetPool&&) = delete;

    /// Acquire a target matching desc. Reuses a free matching target if available,
    /// otherwise creates a new one.
    [[nodiscard]] RenderTarget acquire(const RenderTargetDesc& desc);

    /// Return a target to the pool for reuse.
    void release(const RenderTarget& target);

    /// Called at end of frame. Marks all in-use targets as available and
    /// increments unused-frame counters. Destroys targets unused for too long.
    void end_frame();

    /// Destroy all pooled targets (e.g. on viewport resize).
    void reset();

private:
    struct PoolEntry {
        RenderTargetDesc desc;
        RenderTarget target;
        bool free{true};
        uint32_t unused_frames{0};
    };

    [[nodiscard]] bool matches(const PoolEntry& entry, const RenderTargetDesc& desc) const;
    PoolEntry create_entry(const RenderTargetDesc& desc);
    void destroy_entry(PoolEntry& entry);

    VkDevice device_;
    VmaAllocator allocator_;
    std::vector<PoolEntry> entries_;

    static constexpr uint32_t MAX_UNUSED_FRAMES = 8;
};

} // namespace fjell
