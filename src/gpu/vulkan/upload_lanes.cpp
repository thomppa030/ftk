#include "gpu/vulkan/upload_lanes.hpp"

#include "core/log.hpp"
#include "gpu/vulkan/foundation.hpp"
#include "gpu/vulkan/vk_check.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace fjell::gpu::vulkan {

UploadLanes::Staging::Staging(VmaAllocator made_by, VkDeviceSize size) : allocator{made_by} {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_info.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo made{};
    vk_check(vmaCreateBuffer(allocator, &buffer_info, &allocation_info, &buffer, &allocation, &made),
             "Failed to create upload staging memory");
    mapped = made.pMappedData;
}

UploadLanes::Staging::~Staging() {
    vmaDestroyBuffer(allocator, buffer, allocation);
}

UploadLanes::UploadLanes(Foundation& foundation)
    : device_{foundation.handle()}
    , allocator_{foundation.allocator()}
    , sharing_families_{foundation.upload_sharing_families()} {
    lanes_[IMAGE].queue = foundation.graphics_queue();
    lanes_[BUFFER].queue = foundation.transfer_queue_supported()
        ? foundation.transfer_queue() : foundation.graphics_queue();

    const auto& indices = foundation.queue_families();
    std::array<uint32_t, LANE_COUNT> lane_families{};
    lane_families[IMAGE] = indices.graphics.value();
    lane_families[BUFFER] = indices.transfer.value_or(indices.graphics.value());

    for (uint32_t i = 0; i < LANE_COUNT; ++i) {
        VkCommandPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_ci.queueFamilyIndex = lane_families[i];
        vk_check(vkCreateCommandPool(device_, &pool_ci, nullptr, &lanes_[i].pool),
                 "Failed to create upload command pool");

        VkSemaphoreTypeCreateInfo tl_ci{};
        tl_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        tl_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        tl_ci.initialValue = 0;
        VkSemaphoreCreateInfo sem_ci{};
        sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sem_ci.pNext = &tl_ci;
        vk_check(vkCreateSemaphore(device_, &sem_ci, nullptr, &lanes_[i].timeline),
                 "Failed to create upload timeline semaphore");
    }

    ring_capacity_ = RING_CAPACITY;
    ring_ = std::make_unique<Staging>(allocator_, ring_capacity_);

    FJELL_GFX_INFO("Upload lanes: {}MB staging ring, buffer lane on {} queue",
                   ring_capacity_ / (1024 * 1024),
                   foundation.transfer_queue_supported() ? "transfer" : "graphics");
}

UploadLanes::~UploadLanes() {
    wait_all();
    reclaim_completed();
    for (auto& lane : lanes_) {
        if (lane.timeline != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, lane.timeline, nullptr);
        }
        if (lane.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, lane.pool, nullptr);
        }
    }
}

void UploadLanes::upload_buffer(VkBuffer dst, VkDeviceSize dst_offset,
                                const void* data, VkDeviceSize size) {
    if (size == 0) return;
    StagingSlice slice = acquire_staging(BUFFER, size);
    std::memcpy(slice.ptr, data, size);

    VkBufferCopy region{};
    region.srcOffset = slice.offset;
    region.dstOffset = dst_offset;
    region.size = size;
    vkCmdCopyBuffer(lane_cb(BUFFER), slice.buffer, dst, 1, &region);
}

void UploadLanes::copy_buffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
    if (size == 0) return;
    VkCommandBuffer cmd = lane_cb(BUFFER);

    // Order this copy after every transfer recorded so far in the lane —
    // growth copies read from buffers that may have received uploads
    // earlier in this same batch or a still-in-flight one.
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);
}

StagingSlice UploadLanes::stage_for_image(const void* data, VkDeviceSize size) {
    StagingSlice slice = acquire_staging(IMAGE, size);
    std::memcpy(slice.ptr, data, size);
    return slice;
}

VkCommandBuffer UploadLanes::image_cb() {
    return lane_cb(IMAGE);
}

std::shared_ptr<const uint64_t> UploadLanes::image_batch() {
    (void)lane_cb(IMAGE);
    return lanes_[IMAGE].open.value;
}

bool UploadLanes::image_done(uint64_t value) const {
    return value != 0 && completed_value(IMAGE) >= value;
}

void UploadLanes::wait_image(uint64_t value) {
    VkSemaphoreWaitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait.semaphoreCount = 1;
    wait.pSemaphores = &lanes_[IMAGE].timeline;
    wait.pValues = &value;
    vk_check(vkWaitSemaphores(device_, &wait, UINT64_MAX), "Failed waiting for the image lane");
}

VkCommandBuffer UploadLanes::lane_cb(LaneIndex lane) {
    Batch& open = lanes_[lane].open;
    if (open.cb == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = lanes_[lane].pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(device_, &alloc_info, &open.cb),
                 "Failed to allocate upload command buffer");

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(open.cb, &begin),
                 "Failed to begin upload command buffer");

        open.value = std::make_shared<uint64_t>(0);
    }
    return open.cb;
}

StagingSlice UploadLanes::acquire_staging(LaneIndex lane, VkDeviceSize size) {
    // Make sure the batch (and its value slot) exists before any allocation
    // references it.
    (void)lane_cb(lane);

    // Oversized requests bypass the ring entirely; the dedicated buffer
    // rides with the batch and is freed when it completes.
    if (size > ring_capacity_ / 2) {
        auto dedicated = std::make_unique<Staging>(allocator_, size);
        StagingSlice slice{dedicated->buffer, 0, dedicated->mapped};
        lanes_[lane].open.dedicated.push_back(std::move(dedicated));
        return slice;
    }

    VkDeviceSize aligned = (size + RING_ALIGNMENT - 1) & ~(RING_ALIGNMENT - 1);

    // Live data occupies [tail_, head_), possibly wrapped. An empty
    // allocation deque means the ring is idle; equal cursors with live
    // allocations mean full.
    for (;;) {
        reclaim_completed();

        if (ring_allocs_.empty()) {
            head_ = 0;
            tail_ = 0;
            break;
        }
        if (head_ > tail_) {
            if (ring_capacity_ - head_ >= aligned) {
                break;
            }
            if (tail_ > aligned) {
                // Wrap: hand the dead tail-end region to the open batch so
                // the reclaim cursor walks past it in order. flush() above
                // may have retired and reset this lane's batch since we
                // last opened it, so re-open before reading its value —
                // otherwise this pushes a sentinel with a null batch_value
                // that reclaim_completed can never resolve.
                (void)lane_cb(lane);
                ring_allocs_.push_back({ring_capacity_, lane, lanes_[lane].open.value});
                head_ = 0;
                break;
            }
        } else if (head_ < tail_ && tail_ - head_ > aligned) {
            break;
        }

        // Full or fragmented: submit what we have, then wait for the
        // oldest batch to release its region.
        flush();
        const RingAlloc& oldest = ring_allocs_.front();
        if (*oldest.batch_value == 0) {
            throw std::runtime_error(
                "Staging ring stuck: oldest allocation belongs to an unsubmitted batch");
        }
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &lanes_[oldest.lane].timeline;
        wait.pValues = &*oldest.batch_value;
        vk_check(vkWaitSemaphores(device_, &wait, UINT64_MAX),
                 "Failed waiting for staging ring space");
    }

    StagingSlice slice{ring_->buffer, head_, static_cast<uint8_t*>(ring_->mapped) + head_};
    head_ += aligned;
    // flush() inside the loop above may have retired and reset this lane's
    // batch since we last opened it — re-open before reading its value.
    (void)lane_cb(lane);
    ring_allocs_.push_back({head_, lane, lanes_[lane].open.value});
    return slice;
}

void UploadLanes::flush_lane(LaneIndex lane) {
    Batch& open = lanes_[lane].open;
    if (open.cb == VK_NULL_HANDLE) return;

    vk_check(vkEndCommandBuffer(open.cb), "Failed to end upload command buffer");

    // Non-coherent host memory: make the staged bytes visible to the GPU.
    // No-op on the coherent BAR/system memory VMA usually picks.
    vmaFlushAllocation(allocator_, ring_->allocation, 0, VK_WHOLE_SIZE);
    for (auto& dedicated : open.dedicated) {
        vmaFlushAllocation(allocator_, dedicated->allocation, 0, VK_WHOLE_SIZE);
    }

    uint64_t value = ++lanes_[lane].submitted_value;

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = open.cb;

    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = lanes_[lane].timeline;
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    vk_check(vkQueueSubmit2(lanes_[lane].queue, 1, &submit, VK_NULL_HANDLE),
             "Failed to submit upload batch");

    *open.value = value;
    lanes_[lane].in_flight.push_back(std::move(open));
    open = Batch{};
}

void UploadLanes::flush() {
    flush_lane(BUFFER);
    flush_lane(IMAGE);
    reclaim_completed();
}

void UploadLanes::wait_all() {
    flush();
    for (auto& lane : lanes_) {
        if (lane.submitted_value == 0) continue;
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &lane.timeline;
        wait.pValues = &lane.submitted_value;
        vk_check(vkWaitSemaphores(device_, &wait, UINT64_MAX),
                 "Failed waiting for uploads");
    }
    reclaim_completed();
}

uint64_t UploadLanes::completed_value(LaneIndex lane) const {
    uint64_t value = 0;
    vk_check(vkGetSemaphoreCounterValue(device_, lanes_[lane].timeline, &value),
             "Failed to query upload timeline");
    return value;
}

void UploadLanes::reclaim_completed() {
    std::array<uint64_t, LANE_COUNT> completed{completed_value(BUFFER), completed_value(IMAGE)};

    // Ring allocations reclaim strictly in ring order; an open batch
    // (*batch_value == 0) blocks the cursor until it flushes and completes.
    while (!ring_allocs_.empty()) {
        const RingAlloc& front = ring_allocs_.front();
        uint64_t value = *front.batch_value;
        if (value == 0 || completed[front.lane] < value) break;
        tail_ = front.end == ring_capacity_ ? 0 : front.end;
        ring_allocs_.pop_front();
    }

    for (uint32_t i = 0; i < LANE_COUNT; ++i) {
        auto& lane = lanes_[i];
        while (!lane.in_flight.empty() && completed[i] >= *lane.in_flight.front().value) {
            vkFreeCommandBuffers(device_, lane.pool, 1, &lane.in_flight.front().cb);
            lane.in_flight.pop_front();
        }
    }
}

} // namespace fjell::gpu::vulkan
