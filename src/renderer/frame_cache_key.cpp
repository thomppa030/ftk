#include "renderer/frame_descriptor_cache.hpp"

// Pure hashing / equality for FrameCacheKey. Lives in its own .cpp so
// fjell-testable can link the logic without pulling in the full
// FrameDescriptorCache (which touches VkDescriptorPool and requires a
// real Vulkan device).

namespace fjell {

namespace {

size_t hash_image_info(const VkDescriptorImageInfo& info) {
    size_t h = std::hash<void*>{}(static_cast<void*>(info.sampler));
    h ^= std::hash<void*>{}(static_cast<void*>(info.imageView)) << 1;
    h ^= std::hash<int>{}(static_cast<int>(info.imageLayout)) << 2;
    return h;
}

size_t hash_buffer_info(const VkDescriptorBufferInfo& info) {
    size_t h = std::hash<void*>{}(static_cast<void*>(info.buffer));
    h ^= std::hash<uint64_t>{}(info.offset) << 1;
    h ^= std::hash<uint64_t>{}(info.range) << 2;
    return h;
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

bool FrameCacheKey::operator==(const FrameCacheKey& o) const noexcept {
    if (layout != o.layout) { return false; }
    if (bindings.size() != o.bindings.size()) { return false; }
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& a = bindings[i];
        const auto& b = o.bindings[i];
        if (a.binding != b.binding || a.type != b.type) { return false; }
        if (is_image_descriptor(a.type)) {
            if (!image_info_equal(a.image, b.image)) { return false; }
        } else {
            if (!buffer_info_equal(a.buffer, b.buffer)) { return false; }
        }
    }
    return true;
}

size_t FrameCacheKeyHash::operator()(const FrameCacheKey& k) const noexcept {
    size_t h = std::hash<void*>{}(static_cast<void*>(k.layout));
    for (const auto& b : k.bindings) {
        h ^= std::hash<uint32_t>{}(b.binding) << 1;
        h ^= std::hash<int>{}(static_cast<int>(b.type)) << 2;
        if (is_image_descriptor(b.type)) {
            h ^= hash_image_info(b.image) << 3;
        } else {
            h ^= hash_buffer_info(b.buffer) << 3;
        }
    }
    return h;
}

} // namespace fjell
