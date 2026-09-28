#include "gpu/vulkan/frame_descriptor_cache.hpp"

// Pure hashing / equality for FrameCacheKey. Lives in its own .cpp so
// fjell-testable can link the logic without pulling in the full
// FrameDescriptorCache (which touches VkDescriptorPool and requires a
// real Vulkan device).

namespace fjell {

namespace {

// Mixes `value` into `seed` so equal parts in other places hash apart.
size_t combine(size_t seed, size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

size_t hash_image_info(const VkDescriptorImageInfo& info) {
    size_t h = std::hash<void*>{}(static_cast<void*>(info.sampler));
    h = combine(h, std::hash<void*>{}(static_cast<void*>(info.imageView)));
    return combine(h, std::hash<int>{}(static_cast<int>(info.imageLayout)));
}

size_t hash_buffer_info(const VkDescriptorBufferInfo& info) {
    size_t h = std::hash<void*>{}(static_cast<void*>(info.buffer));
    h = combine(h, std::hash<uint64_t>{}(info.offset));
    return combine(h, std::hash<uint64_t>{}(info.range));
}

bool image_info_equal(const VkDescriptorImageInfo& a, const VkDescriptorImageInfo& b) {
    return a.sampler == b.sampler && a.imageView == b.imageView &&
           a.imageLayout == b.imageLayout;
}

bool buffer_info_equal(const VkDescriptorBufferInfo& a, const VkDescriptorBufferInfo& b) {
    return a.buffer == b.buffer && a.offset == b.offset && a.range == b.range;
}

bool is_image_descriptor(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_SAMPLER;
}

} // namespace

bool FrameCacheKeyView::operator==(const FrameCacheKeyView& o) const noexcept {
    if (layout != o.layout) { return false; }
    if (bindings.size() != o.bindings.size()) { return false; }
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& a = bindings[i];
        const auto& b = o.bindings[i];
        if (a.binding != b.binding || a.element != b.element || a.type != b.type) { return false; }
        if (is_image_descriptor(a.type)) {
            if (!image_info_equal(a.image, b.image)) { return false; }
        } else {
            if (!buffer_info_equal(a.buffer, b.buffer)) { return false; }
        }
    }
    return true;
}

size_t FrameCacheKeyHash::operator()(const FrameCacheKeyView& k) const noexcept {
    size_t h = std::hash<void*>{}(static_cast<void*>(k.layout));
    for (const auto& b : k.bindings) {
        h = combine(h, std::hash<uint32_t>{}(b.binding));
        h = combine(h, std::hash<uint32_t>{}(b.element));
        h = combine(h, std::hash<int>{}(static_cast<int>(b.type)));
        h = combine(h, is_image_descriptor(b.type) ? hash_image_info(b.image)
                                                   : hash_buffer_info(b.buffer));
    }
    return h;
}

} // namespace fjell
