#include "renderer/render_target_pool.hpp"
#include "renderer/gpu/buffer.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <algorithm>

namespace fjell {

RenderTargetPool::RenderTargetPool(VkDevice device, VkPhysicalDevice physical_device)
    : device_{device}, physical_device_{physical_device} {}

RenderTargetPool::~RenderTargetPool() {
    reset();
}

RenderTarget RenderTargetPool::acquire(const RenderTargetDesc& desc) {
    // Look for a free entry that matches
    for (auto& entry : entries_) {
        if (entry.free && matches(entry, desc)) {
            entry.free = false;
            entry.unused_frames = 0;
            return entry.target;
        }
    }

    // No match — create a new entry
    auto entry = create_entry(desc);
    entry.free = false;
    entries_.push_back(entry);
    return entries_.back().target;
}

void RenderTargetPool::release(const RenderTarget& target) {
    for (auto& entry : entries_) {
        if (entry.target.image == target.image) {
            entry.free = true;
            return;
        }
    }
}

void RenderTargetPool::end_frame() {
    for (auto& entry : entries_) {
        if (!entry.free) {
            entry.free = true;
            entry.unused_frames = 0;
        } else {
            entry.unused_frames++;
        }
    }

    // Trim targets unused for too many frames
    std::erase_if(entries_, [this](PoolEntry& entry) {
        if (entry.free && entry.unused_frames >= MAX_UNUSED_FRAMES) {
            destroy_entry(entry);
            return true;
        }
        return false;
    });
}

void RenderTargetPool::reset() {
    for (auto& entry : entries_) {
        destroy_entry(entry);
    }
    entries_.clear();
}

bool RenderTargetPool::matches(const PoolEntry& entry, const RenderTargetDesc& desc) const {
    return entry.desc.width == desc.width
        && entry.desc.height == desc.height
        && entry.desc.format == desc.format
        && entry.desc.samples == desc.samples
        && entry.desc.usage == desc.usage
        && entry.desc.aspect == desc.aspect;
}

RenderTargetPool::PoolEntry RenderTargetPool::create_entry(const RenderTargetDesc& desc) {
    PoolEntry entry;
    entry.desc = desc;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = desc.format;
    image_info.extent = {desc.width, desc.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = desc.samples;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = desc.usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vk_check(vkCreateImage(device_, &image_info, nullptr, &entry.target.image),
             "create pool render target image");

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device_, entry.target.image, &mem_reqs);

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = mem_reqs.size;
    alloc.memoryTypeIndex = Buffer::find_memory_type(
        physical_device_, mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vk_check(vkAllocateMemory(device_, &alloc, nullptr, &entry.target.memory),
             "allocate pool render target memory");
    vk_check(vkBindImageMemory(device_, entry.target.image, entry.target.memory, 0),
             "bind pool render target memory");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = entry.target.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = desc.format;
    view_info.subresourceRange = {desc.aspect, 0, 1, 0, 1};

    vk_check(vkCreateImageView(device_, &view_info, nullptr, &entry.target.view),
             "create pool render target view");

    entry.target.extent = {desc.width, desc.height};
    entry.target.format = desc.format;

    return entry;
}

void RenderTargetPool::destroy_entry(PoolEntry& entry) {
    if (entry.target.view != VK_NULL_HANDLE)
        vkDestroyImageView(device_, entry.target.view, nullptr);
    if (entry.target.image != VK_NULL_HANDLE)
        vkDestroyImage(device_, entry.target.image, nullptr);
    if (entry.target.memory != VK_NULL_HANDLE)
        vkFreeMemory(device_, entry.target.memory, nullptr);
    entry.target = {};
}

} // namespace fjell
