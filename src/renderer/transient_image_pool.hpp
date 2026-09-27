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
/// Lifecycle: an image acquired in one frame is free again once that
/// frame's fence has been waited on, which begin_frame() is told through
/// the completed serial; a frame still in flight keeps its images, so a
/// new frame never writes what an older one is still reading. Within a
/// frame, acquire() pulls a free matching image (allocating on miss) and
/// every run of that frame gets its own. An image no frame has wanted
/// for a while is destroyed in begin_frame(), once its last frame is
/// complete, which is what reclaims a resolution nothing renders at any
/// more.
class TransientImagePool {
public:
    struct Allocation {
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation memory{VK_NULL_HANDLE};
        VkImageView full_view{VK_NULL_HANDLE};  // whole-image view matching desc
        VkExtent3D extent{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        VkImageUsageFlags usage_flags{0};
        VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
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

    /// Once per frame. `frame_serial` names the frame about to record and
    /// `completed_serial` the newest frame whose fence has been waited on:
    /// images last acquired by that frame or earlier are free again, and
    /// ones idle for STALE_FRAMES are destroyed.
    void begin_frame(uint64_t frame_serial, uint64_t completed_serial);

    /// Take an image matching the resolved desc. Allocates via VMA with
    /// VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT on miss. The image is the
    /// caller's until the frame begin_frame() was last told about is
    /// complete.
    [[nodiscard]] const Allocation* acquire(const TextureDesc& desc,
                                            VkExtent2D viewport_extent,
                                            VkImageUsageFlags usage_flags);

    /// The extent an image of `desc` has at `viewport`. A persistent image
    /// that must line up with a transient of the same desc sizes itself
    /// through this rather than repeating the arithmetic.
    [[nodiscard]] static VkExtent3D resolve_extent(const TextureDesc& desc,
                                                   VkExtent2D viewport);

    /// Frames an image may go unused before it is destroyed.
    static constexpr uint64_t STALE_FRAMES = 120;

    // Stats
    [[nodiscard]] uint32_t live_images() const noexcept;
    [[nodiscard]] uint32_t acquires_this_frame() const noexcept { return acquires_this_frame_; }
    [[nodiscard]] uint32_t allocations_this_frame() const noexcept { return allocations_this_frame_; }
    [[nodiscard]] uint64_t live_bytes() const noexcept { return live_bytes_; }

private:
    struct Entry {
        Allocation alloc;
        VkDeviceSize bytes{0};
        uint64_t last_used{0};  ///< frame serial of the last acquire; 0 = never
    };

    [[nodiscard]] bool entry_matches(const Entry& e,
                                      const TextureDesc& desc,
                                      VkExtent3D resolved,
                                      VkImageUsageFlags usage) const;
    void destroy_entry(Entry& e);

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    std::vector<Entry> entries_;
    uint64_t frame_serial_{0};
    uint64_t completed_serial_{0};

    uint32_t acquires_this_frame_{0};
    uint32_t allocations_this_frame_{0};
    uint64_t live_bytes_{0};
};

} // namespace fjell
