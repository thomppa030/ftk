#include "gpu/vulkan/frame_descriptor_cache.hpp"

#include "core/log.hpp"
#include "renderer/gpu/vk_check.hpp"

#include <array>
#include <cstring>

namespace fjell {

namespace {

VkDescriptorPool create_pool(VkDevice device, const FrameCachePoolBudget& budget) {
    std::array<VkDescriptorPoolSize, 4> sizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, budget.sampled_images},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          budget.storage_images},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         budget.uniform_buffers},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         budget.storage_buffers},
    }};

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = budget.max_sets;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    // No FREE_DESCRIPTOR_SET_BIT — we rely on vkResetDescriptorPool at
    // begin_frame for bulk reclaim. Free-individual is slower and wasted
    // here.

    VkDescriptorPool pool = VK_NULL_HANDLE;
    vk_check(vkCreateDescriptorPool(device, &info, nullptr, &pool),
             "FrameDescriptorCache: create pool");
    return pool;
}

bool is_image_descriptor(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_SAMPLER;
}

} // namespace

void FrameDescriptorCache::create(VkDevice device, uint32_t frames_in_flight,
                                   const FrameCachePoolBudget& budget) {
    device_ = device;
    budget_ = budget;
    slots_.resize(frames_in_flight);
    for (auto& slot : slots_) {
        slot.primary = create_pool(device_, budget_);
    }
}

void FrameDescriptorCache::destroy() {
    if (device_ == VK_NULL_HANDLE) { return; }
    for (auto& slot : slots_) {
        for (auto pool : slot.overflow) {
            vkDestroyDescriptorPool(device_, pool, nullptr);
        }
        if (slot.primary != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, slot.primary, nullptr);
        }
        slot.overflow.clear();
        slot.primary = VK_NULL_HANDLE;
        slot.cache.clear();
    }
    slots_.clear();
    device_ = VK_NULL_HANDLE;
}

void FrameDescriptorCache::begin_frame(uint32_t frame_index) {
    // Roll over stats from the frame we just finished.
    stats_.sets_last_frame = stats_.sets_this_frame;
    stats_.hits_last_frame = stats_.hits_this_frame;
    stats_.overflows_last_frame = stats_.overflows_this_frame;
    stats_.sets_this_frame = 0;
    stats_.hits_this_frame = 0;
    stats_.overflows_this_frame = 0;

    current_frame_ = frame_index;
    auto& slot = slots_[current_frame_];

    // Reset primary pool (bulk reclaim). Overflow pools are destroyed;
    // they were allocated on demand last frame and we'd rather reclaim
    // the memory than keep them around for uneven usage patterns.
    vk_check(vkResetDescriptorPool(device_, slot.primary, 0),
             "FrameDescriptorCache: reset pool");
    for (auto pool : slot.overflow) {
        vkDestroyDescriptorPool(device_, pool, nullptr);
    }
    slot.overflow.clear();
    slot.cache.clear();
}

VkDescriptorPool FrameDescriptorCache::allocate_overflow_pool() {
    auto& slot = slots_[current_frame_];
    VkDescriptorPool pool = create_pool(device_, budget_);
    slot.overflow.push_back(pool);
    ++stats_.overflows_this_frame;
    FJELL_GFX_WARN("FrameDescriptorCache: overflow pool allocated "
                   "(frame={}, total overflow={}); consider raising the budget",
                   current_frame_,
                   static_cast<unsigned>(slot.overflow.size()));
    return pool;
}

VkDescriptorSet FrameDescriptorCache::try_allocate(VkDescriptorPool pool,
                                                    VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult r = vkAllocateDescriptorSets(device_, &info, &set);
    if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) {
        return VK_NULL_HANDLE;
    }
    vk_check(r, "FrameDescriptorCache: allocate descriptor set");
    return set;
}

void FrameDescriptorCache::write_bindings(VkDescriptorSet set,
                                           std::span<const FrameCacheBinding> bindings) {
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(bindings.size());
    for (const auto& b : bindings) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = b.binding;
        w.descriptorCount = 1;
        w.descriptorType = b.type;
        if (is_image_descriptor(b.type)) {
            w.pImageInfo = &b.image;
        } else {
            w.pBufferInfo = &b.buffer;
        }
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()),
                            writes.data(), 0, nullptr);
}

VkDescriptorSet FrameDescriptorCache::acquire(VkDescriptorSetLayout layout,
                                               std::span<const FrameCacheBinding> bindings) {
    auto& slot = slots_[current_frame_];

    FrameCacheKey key{};
    key.layout = layout;
    key.bindings.assign(bindings.begin(), bindings.end());

    auto it = slot.cache.find(key);
    if (it != slot.cache.end()) {
        ++stats_.hits_this_frame;
        return it->second;
    }

    // Try primary pool first, then existing overflow pools, then allocate
    // a new overflow pool. Keeps the common path fast while handling
    // unexpected allocation pressure.
    VkDescriptorSet set = try_allocate(slot.primary, layout);
    if (set == VK_NULL_HANDLE) {
        for (auto pool : slot.overflow) {
            set = try_allocate(pool, layout);
            if (set != VK_NULL_HANDLE) { break; }
        }
    }
    if (set == VK_NULL_HANDLE) {
        VkDescriptorPool overflow = allocate_overflow_pool();
        set = try_allocate(overflow, layout);
    }
    if (set == VK_NULL_HANDLE) {
        FJELL_GFX_ERROR("FrameDescriptorCache: failed to allocate descriptor set "
                        "even after overflow pool");
        return VK_NULL_HANDLE;
    }

    write_bindings(set, bindings);
    slot.cache.emplace(std::move(key), set);
    ++stats_.sets_this_frame;
    return set;
}

} // namespace fjell
