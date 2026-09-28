#include "renderer/gpu/growable_buffer.hpp"
#include "gpu/vulkan/native.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/gpu/device.hpp"
#include "renderer/gpu/upload_context.hpp"
#include "renderer/gpu/vk_check.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace fjell {

GrowableBuffer::GrowableBuffer(Device& device, VmaAllocator allocator,
                               UploadContext& upload, DeferredDeleter& deleter,
                               VkBufferUsageFlags usage,
                               VkDeviceSize initial_capacity,
                               VkDeviceSize virtual_capacity,
                               std::string debug_name)
    : device_{device.handle()}
    , allocator_{allocator}
    , upload_{upload}
    , deleter_{deleter}
    , usage_{usage}
    , virtual_capacity_{virtual_capacity}
    , name_{std::move(debug_name)} {
    if (!device.sparse_binding_supported()) {
        fallback_ = std::make_unique<Buffer>(
            allocator_, initial_capacity, usage_,
            VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());
        capacity_ = initial_capacity;
        return;
    }

    bind_queue_ = device.sparse_bind_queue();

    VkBufferCreateInfo buffer_ci{};
    buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_ci.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT;
    buffer_ci.size = virtual_capacity_;
    buffer_ci.usage = usage_;
    auto sharing = upload_.sharing_families();
    if (sharing.size() >= 2) {
        buffer_ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        buffer_ci.queueFamilyIndexCount = static_cast<uint32_t>(sharing.size());
        buffer_ci.pQueueFamilyIndices = sharing.data();
    } else {
        buffer_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    vk_check(vkCreateBuffer(device_, &buffer_ci, nullptr, &sparse_buffer_),
             "Failed to create sparse buffer");

    // alignment is the sparse block size — every bind must start and end on
    // a block boundary (except at the buffer's tail).
    vkGetBufferMemoryRequirements(device_, sparse_buffer_, &memory_requirements_);

    // bind_pages() always rounds its target up to a multiple of alignment,
    // then clamps to virtual_capacity_ — if virtual_capacity_ itself isn't
    // a multiple of alignment, that clamp can land on an unaligned value,
    // producing an invalid vkQueueBindSparse call. Catch that here, at
    // construction, instead of downstream as a validation error on the
    // first bind — a caller sizing a small buffer (e.g. a handful of
    // uints per emitter) can easily end up under one sparse block (64KB
    // is typical) without realizing sparse mode has a practical minimum.
    if (virtual_capacity_ % memory_requirements_.alignment != 0) {
        throw std::runtime_error("GrowableBuffer '" + name_ +
                                 "': virtual_capacity (" + std::to_string(virtual_capacity_) +
                                 ") must be a multiple of the sparse block alignment (" +
                                 std::to_string(memory_requirements_.alignment) + ")");
    }

    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vk_check(vkCreateFence(device_, &fence_ci, nullptr, &bind_fence_),
             "Failed to create sparse bind fence");

    bind_pages(initial_capacity);

    FJELL_GFX_DEBUG("GrowableBuffer '{}': sparse, {}MB committed of {}MB virtual",
                    name_, capacity_ / (1024 * 1024), virtual_capacity_ / (1024 * 1024));
}

GrowableBuffer::~GrowableBuffer() {
    if (sparse_buffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, sparse_buffer_, nullptr);
        if (!pages_.empty()) {
            vmaFreeMemoryPages(allocator_, pages_.size(), pages_.data());
        }
        vkDestroyFence(device_, bind_fence_, nullptr);
    }
}

void GrowableBuffer::ensure_capacity(VkDeviceSize needed) {
    if (needed <= capacity_) return;

    if (sparse_buffer_ != VK_NULL_HANDLE) {
        if (needed > virtual_capacity_) {
            throw std::runtime_error("GrowableBuffer '" + name_ +
                                     "' exceeded its virtual capacity");
        }
        bind_pages(std::min(std::max(needed, capacity_ * 2), virtual_capacity_));
        FJELL_GFX_INFO("GrowableBuffer '{}': committed {}MB (sparse, address stable)",
                       name_, capacity_ / (1024 * 1024));
        return;
    }

    // Fallback: reallocate and copy. The copy rides the upload context's
    // buffer lane, and in-flight frames keep reading the old buffer until
    // their fence signals — retire it, don't destroy it.
    VkDeviceSize new_size = needed * 2;
    auto new_buffer = std::make_unique<Buffer>(
        allocator_, new_size, usage_,
        VMA_MEMORY_USAGE_AUTO, 0, 0, upload_.sharing_families());
    upload_.copy_buffer(fallback_->handle(), new_buffer->handle(), capacity_);
    deleter_.retire(std::move(fallback_));
    fallback_ = std::move(new_buffer);
    if (shared_with_ != nullptr) fallback_->share(*shared_with_);
    capacity_ = new_size;
    ++generation_;
    FJELL_GFX_INFO("GrowableBuffer '{}': grew to {}MB (copy, new address)",
                   name_, capacity_ / (1024 * 1024));
}

void GrowableBuffer::share(gpu::Device& device) {
    shared_with_ = &device;
    if (sparse_buffer_ != VK_NULL_HANDLE) {
        sparse_shared_ = gpu::vulkan::adopt(device, sparse_buffer_, virtual_capacity_);
    } else {
        fallback_->share(device);
    }
}

void GrowableBuffer::bind_pages(VkDeviceSize target) {
    VkDeviceSize granularity = memory_requirements_.alignment;
    target = std::min((target + granularity - 1) & ~(granularity - 1), virtual_capacity_);

    VkMemoryRequirements page_requirements = memory_requirements_;
    page_requirements.size = target - capacity_;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VmaAllocation page{VK_NULL_HANDLE};
    VmaAllocationInfo page_info{};
    vk_check(vmaAllocateMemoryPages(allocator_, &page_requirements, &alloc_ci,
                                    1, &page, &page_info),
             "Failed to allocate sparse buffer pages");

    VkSparseMemoryBind bind{};
    bind.resourceOffset = capacity_;
    bind.size = page_requirements.size;
    bind.memory = page_info.deviceMemory;
    bind.memoryOffset = page_info.offset;

    VkSparseBufferMemoryBindInfo buffer_bind{};
    buffer_bind.buffer = sparse_buffer_;
    buffer_bind.bindCount = 1;
    buffer_bind.pBinds = &bind;

    VkBindSparseInfo bind_info{};
    bind_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
    bind_info.bufferBindCount = 1;
    bind_info.pBufferBinds = &buffer_bind;

    // The bound range is brand-new — no submitted work touches it, so no
    // semaphore chaining is needed, just a CPU-visible completion point.
    // Sparse binds order only against semaphores (never command buffers),
    // so this fence signals in microseconds even with frames in flight.
    vk_check(vkResetFences(device_, 1, &bind_fence_), "Failed to reset bind fence");
    vk_check(vkQueueBindSparse(bind_queue_, 1, &bind_info, bind_fence_),
             "Failed to bind sparse buffer pages");
    vk_check(vkWaitForFences(device_, 1, &bind_fence_, VK_TRUE, UINT64_MAX),
             "Failed waiting for sparse bind");

    pages_.push_back(page);
    capacity_ = target;
}

} // namespace fjell
