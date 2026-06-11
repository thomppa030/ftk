#pragma once

#include "renderer/gpu/buffer.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace fjell {

class Device;

/// A mapped slice of upload staging memory, already positioned for a copy
/// command (`buffer` + `offset` go straight into VkBufferCopy /
/// VkBufferImageCopy).
struct StagingSlice {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceSize offset{0};
    void* ptr{nullptr};
};

/// Batched, non-blocking GPU uploads.
///
/// Replaces the allocate-staging / submit / vkQueueWaitIdle pattern: callers
/// stage data and record copies, and the batch is submitted once per frame
/// by flush(). Nothing blocks — frame submission waits on the upload
/// timeline instead, and staging memory recycles through a persistent ring
/// reclaimed as batches complete.
///
/// Two lanes:
///  - buffer lane: vkCmdCopyBuffer batches, submitted to the dedicated
///    transfer queue (the DMA engine) when the device has one, so copies
///    overlap rendering. Destination buffers must be created with
///    Device::upload_sharing_families() so their content is defined across
///    queues. Falls back to the graphics queue otherwise.
///  - image lane: always the graphics queue — mip generation blits need a
///    graphics-capable queue, and same-queue submission order makes the
///    in-CB layout transitions sufficient sync.
///
/// Main-thread only, like the rest of the GPU submission paths.
class UploadContext {
public:
    UploadContext(Device& device, VmaAllocator allocator);
    ~UploadContext();

    UploadContext(const UploadContext&) = delete;
    UploadContext& operator=(const UploadContext&) = delete;
    UploadContext(UploadContext&&) = delete;
    UploadContext& operator=(UploadContext&&) = delete;

    /// Stage `data` and record a copy into `dst` at `dst_offset`.
    /// Overlapping writes to the same destination region within one batch
    /// are unordered — callers append to fresh regions, which is the only
    /// pattern the engine uses.
    void upload_buffer(VkBuffer dst, VkDeviceSize dst_offset,
                       const void* data, VkDeviceSize size);

    /// Record a GPU→GPU copy (buffer growth). Ordered after all previously
    /// recorded uploads via a transfer→transfer barrier, so copying out of
    /// a buffer that received data earlier in the batch is safe.
    void copy_buffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);

    /// Stage pixel data for an image upload. Record the layout transitions,
    /// vkCmdCopyBufferToImage, and any mip blits into image_cb().
    [[nodiscard]] StagingSlice stage_for_image(const void* data, VkDeviceSize size);

    /// The image lane's open command buffer (graphics queue). Valid until
    /// the next flush().
    [[nodiscard]] VkCommandBuffer image_cb();

    /// Submit both lanes' open batches. Non-blocking; called once per frame
    /// before frame submission, which waits on the timelines.
    void flush();

    /// Flush, then CPU-wait until every submitted upload completed. For the
    /// rare paths that read upload destinations through their own one-shot
    /// submits (BLAS builds, editor previews).
    void wait_all();

    /// Timeline the frame submit must wait on when the buffer lane runs on
    /// a separate queue. Waiting last_buffer_value() is always safe — the
    /// value is only ever signaled by already-submitted batches.
    [[nodiscard]] VkSemaphore buffer_timeline() const { return lanes_[BUFFER].timeline; }
    [[nodiscard]] uint64_t last_buffer_value() const { return lanes_[BUFFER].submitted_value; }
    [[nodiscard]] bool buffer_lane_is_separate_queue() const {
        return lanes_[BUFFER].queue != lanes_[IMAGE].queue;
    }

    /// Queue families upload-destination buffers must be shared across
    /// (Device::upload_sharing_families, forwarded so callers creating
    /// destination buffers don't also need the Device).
    [[nodiscard]] std::span<const uint32_t> sharing_families() const {
        return sharing_families_;
    }

private:
    enum LaneIndex : uint32_t { BUFFER = 0, IMAGE = 1, LANE_COUNT = 2 };

    struct Batch {
        VkCommandBuffer cb{VK_NULL_HANDLE};
        // 0 while the batch is open; set to the signaled timeline value at
        // flush. Shared with this batch's ring allocations so reclaim can
        // tell "still recording" from "in flight".
        std::shared_ptr<uint64_t> value{};
        // Oversized staging that bypassed the ring; freed when the batch
        // completes.
        std::vector<std::unique_ptr<Buffer>> dedicated;
    };

    struct Lane {
        VkQueue queue{VK_NULL_HANDLE};
        VkCommandPool pool{VK_NULL_HANDLE};
        VkSemaphore timeline{VK_NULL_HANDLE};
        uint64_t submitted_value{0};
        Batch open{};
        std::deque<Batch> in_flight;
    };

    // One ring allocation, in ring order. Reclaimable once its batch was
    // flushed (*batch_value != 0) and the lane timeline reached that value.
    struct RingAlloc {
        VkDeviceSize end{0};
        LaneIndex lane{BUFFER};
        std::shared_ptr<uint64_t> batch_value{};
    };

    [[nodiscard]] VkCommandBuffer lane_cb(LaneIndex lane);
    [[nodiscard]] StagingSlice acquire_staging(LaneIndex lane, VkDeviceSize size);
    void flush_lane(LaneIndex lane);
    void reclaim_completed();
    [[nodiscard]] uint64_t completed_value(LaneIndex lane) const;

    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{VK_NULL_HANDLE};
    // Points into the Device's stable family array.
    std::span<const uint32_t> sharing_families_;

    std::array<Lane, LANE_COUNT> lanes_;

    // Persistent mapped staging ring shared by both lanes.
    std::unique_ptr<Buffer> ring_;
    VkDeviceSize ring_capacity_{0};
    VkDeviceSize head_{0};
    VkDeviceSize tail_{0};
    std::deque<RingAlloc> ring_allocs_;

    static constexpr VkDeviceSize RING_CAPACITY = 64ull * 1024 * 1024;
    // Covers buffer copy alignment (4) and bufferOffset texel alignment
    // for every format the engine uploads.
    static constexpr VkDeviceSize RING_ALIGNMENT = 16;
};

} // namespace fjell
