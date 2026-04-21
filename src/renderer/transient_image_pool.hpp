#pragma once

#include "renderer/resource_desc.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace fjell {

/// Cross-frame pool of VkImages matching specific TextureDesc shapes.
/// Each AliasGroup from FrameGraph::compute_alias_groups() asks the pool
/// for one image — the pool returns a free cached image that matches
/// exactly, or allocates a fresh one via VMA.
///
/// Lifecycle: begin_frame() returns every image to its free list. Within
/// a frame, acquire() pulls from the front of the matching free list
/// (allocating on miss). Images are never freed mid-session — they sit
/// on the free list until a resolution change invalidates them, at which
/// point destroy_stale() drops entries whose resolved extent no longer
/// matches any recent frame.
class TransientImagePool {
public:
    struct Allocation {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView full_view{VK_NULL_HANDLE};  // whole-image view matching desc
        VkExtent3D extent{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkImageUsageFlags usage_flags{0};
        uint32_t mip_levels{1};
        uint32_t array_layers{1};
        VkImageType image_type{VK_IMAGE_TYPE_2D};
        VkImageViewType view_type{VK_IMAGE_VIEW_TYPE_2D};
    };

    TransientImagePool() = default;
    ~TransientImagePool() = default;

    TransientImagePool(const TransientImagePool&) = delete;
    TransientImagePool& operator=(const TransientImagePool&) = delete;
    TransientImagePool(TransientImagePool&&) = delete;
    TransientImagePool& operator=(TransientImagePool&&) = delete;

    void create(VkDevice device, VmaAllocator allocator);
    void destroy();

    /// Mark every entry as available for reuse. Entries whose resolved
    /// extent no longer matches the current viewport are dropped — they
    /// belong to a resolution that's no longer in play.
    void begin_frame(VkExtent2D viewport_extent);

    /// Take an image matching the resolved desc. Allocates via VMA with
    /// VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT on miss. The caller assumes
    /// "ownership" of the image until begin_frame() recycles it.
    [[nodiscard]] const Allocation* acquire(const TextureDesc& desc,
                                            VkExtent2D viewport_extent,
                                            VkImageUsageFlags usage_flags);

    // Stats
    [[nodiscard]] uint32_t live_images() const noexcept;
    [[nodiscard]] uint32_t acquires_this_frame() const noexcept { return acquires_this_frame_; }
    [[nodiscard]] uint32_t allocations_this_frame() const noexcept { return allocations_this_frame_; }
    [[nodiscard]] uint64_t live_bytes() const noexcept { return live_bytes_; }

private:
    struct Entry {
        Allocation alloc;
        bool in_use{false};
        VkExtent2D source_viewport{};  // viewport extent at allocation time
    };

    [[nodiscard]] static VkExtent3D resolve_extent(const TextureDesc& desc,
                                                   VkExtent2D viewport);
    [[nodiscard]] static bool entry_matches(const Entry& e,
                                             const TextureDesc& desc,
                                             VkExtent3D resolved,
                                             VkImageUsageFlags usage);

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    std::vector<Entry> entries_;

    uint32_t acquires_this_frame_{0};
    uint32_t allocations_this_frame_{0};
    uint64_t live_bytes_{0};
};

} // namespace fjell
