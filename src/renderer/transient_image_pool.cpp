#include "renderer/transient_image_pool.hpp"

#include "core/log.hpp"

#include <algorithm>

namespace fjell {

void TransientImagePool::create(VkDevice device, VmaAllocator allocator) {
    device_ = device;
    allocator_ = allocator;
}

void TransientImagePool::destroy() {
    if (allocator_ == VK_NULL_HANDLE) { return; }
    for (auto& e : entries_) { destroy_entry(e); }
    entries_.clear();
    live_bytes_ = 0;
    device_ = VK_NULL_HANDLE;
    allocator_ = VK_NULL_HANDLE;
}

void TransientImagePool::destroy_entry(Entry& e) {
    if (e.alloc.full_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, e.alloc.full_view, nullptr);
    }
    if (e.alloc.image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, e.alloc.image, e.alloc.memory);
    }
    live_bytes_ -= e.bytes;
    e = Entry{};
}

VkExtent3D TransientImagePool::resolve_extent(const TextureDesc& desc,
                                               VkExtent2D viewport) {
    VkExtent3D out{};
    out.depth = desc.depth > 0 ? desc.depth : 1;
    switch (desc.size_class) {
        case SizeClass::absolute:
            out.width = desc.width;
            out.height = desc.height;
            break;
        case SizeClass::viewport: {
            uint32_t div = desc.viewport_divisor > 0 ? desc.viewport_divisor : 1;
            out.width = std::max(1u, viewport.width / div);
            out.height = std::max(1u, viewport.height / div);
            break;
        }
        case SizeClass::match:
            // Match is resolved by the caller (lookup by reference); fall
            // back to viewport-sized so unhandled uses don't allocate 0x0.
            out.width = viewport.width;
            out.height = viewport.height;
            break;
    }
    return out;
}

bool TransientImagePool::entry_matches(const Entry& e,
                                        const TextureDesc& desc,
                                        VkExtent3D resolved,
                                        VkImageUsageFlags usage) const {
    // Free once the frame that last took it is complete. An image never
    // taken (last_used 0) is free.
    return e.last_used <= completed_serial_
        && e.alloc.format == desc.format
        && e.alloc.extent.width == resolved.width
        && e.alloc.extent.height == resolved.height
        && e.alloc.extent.depth == resolved.depth
        && e.alloc.mip_levels == desc.mip_levels
        && e.alloc.array_layers == desc.array_layers
        && e.alloc.samples == desc.samples
        && e.alloc.view_type == desc.view_type
        // Usage must be a superset; a looser cached image still satisfies
        // a tighter request, but if the request needs bits the cache
        // doesn't have we need a new allocation.
        && (e.alloc.usage_flags & usage) == usage;
}

void TransientImagePool::begin_frame(uint64_t frame_serial, uint64_t completed_serial) {
    frame_serial_ = frame_serial;
    completed_serial_ = completed_serial;
    acquires_this_frame_ = 0;
    allocations_this_frame_ = 0;

    // An image nothing has wanted for STALE_FRAMES belongs to a resolution
    // or a viewport that is gone. Its last frame is long complete, so it
    // can go now.
    std::erase_if(entries_, [&](Entry& e) {
        if (e.last_used > completed_serial_) { return false; }
        if (frame_serial_ - e.last_used <= STALE_FRAMES) { return false; }
        destroy_entry(e);
        return true;
    });
}

const TransientImagePool::Allocation*
TransientImagePool::acquire(const TextureDesc& desc,
                             VkExtent2D viewport_extent,
                             VkImageUsageFlags usage_flags) {
    ++acquires_this_frame_;
    const VkExtent3D resolved = resolve_extent(desc, viewport_extent);

    for (auto& e : entries_) {
        if (entry_matches(e, desc, resolved, usage_flags)) {
            e.last_used = frame_serial_;
            return &e.alloc;
        }
    }

    // Miss — allocate fresh.
    ++allocations_this_frame_;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = (desc.view_type == VK_IMAGE_VIEW_TYPE_3D)
        ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = desc.format;
    ci.extent = resolved;
    ci.mipLevels = desc.mip_levels;
    ci.arrayLayers = desc.array_layers;
    ci.samples = desc.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage_flags | desc.extra_usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT;

    Entry entry{};
    VmaAllocationInfo alloc_info{};
    VkResult r = vmaCreateImage(allocator_, &ci, &alloc_ci,
                                 &entry.alloc.image, &entry.alloc.memory, &alloc_info);
    if (r != VK_SUCCESS) {
        FJELL_GFX_ERROR("TransientImagePool: vmaCreateImage failed ({})",
                        static_cast<int>(r));
        return nullptr;
    }
    live_bytes_ += alloc_info.size;

    entry.bytes = alloc_info.size;
    entry.alloc.extent = resolved;
    entry.alloc.format = desc.format;
    entry.alloc.usage_flags = ci.usage;
    entry.alloc.mip_levels = desc.mip_levels;
    entry.alloc.array_layers = desc.array_layers;
    entry.alloc.samples = desc.samples;
    entry.alloc.image_type = ci.imageType;
    entry.alloc.view_type = desc.view_type;
    entry.last_used = frame_serial_;

    // Whole-image view. Exact-match aliasing means every logical
    // resource in a group has the same subresource span, so one view
    // per Entry is sufficient. Per-mip/per-layer views arrive with
    // subresource-aware aliasing later.
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = entry.alloc.image;
    vi.viewType = desc.view_type;
    vi.format = desc.format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = desc.mip_levels;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = desc.array_layers;
    r = vkCreateImageView(device_, &vi, nullptr, &entry.alloc.full_view);
    if (r != VK_SUCCESS) {
        FJELL_GFX_ERROR("TransientImagePool: vkCreateImageView failed ({})",
                        static_cast<int>(r));
        vmaDestroyImage(allocator_, entry.alloc.image, entry.alloc.memory);
        live_bytes_ -= alloc_info.size;
        return nullptr;
    }

    entries_.push_back(entry);
    return &entries_.back().alloc;
}

uint32_t TransientImagePool::live_images() const noexcept {
    return static_cast<uint32_t>(entries_.size());
}

} // namespace fjell
