#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace fjell {

/// One binding inside an acquire() request. Lives on the stack at the
/// call site; the cache copies the value into its key when interning.
struct FrameCacheBinding {
    uint32_t binding{0};
    /// The element of an array binding; 0 for a single one.
    uint32_t element{0};
    VkDescriptorType type{VK_DESCRIPTOR_TYPE_MAX_ENUM};

    // Exactly one of these is populated depending on `type`.
    VkDescriptorImageInfo image{};
    VkDescriptorBufferInfo buffer{};
    VkAccelerationStructureKHR acceleration{VK_NULL_HANDLE};
};

/// Pool budget sized for one in-flight frame. The cache keeps a separate
/// pool per frame slot so reset-on-begin_frame reclaims everything at once.
struct FrameCachePoolBudget {
    uint32_t sampled_images{256};
    /// Textures and samplers bound apart (`texture2D`, `sampler`).
    uint32_t separate_images{64};
    uint32_t samplers{32};
    uint32_t storage_images{128};
    uint32_t uniform_buffers{64};
    uint32_t storage_buffers{128};
    /// None on a GPU without ray queries, whose pools may not name the type.
    uint32_t acceleration_structures{0};
    uint32_t max_sets{512};
};

/// Cache key exposed for testability: hashing and equality are the only
/// correctness-critical logic in FrameDescriptorCache. A wrong hash or
/// equality surfaces as a rendering bug many layers away, so these are
/// kept free of Vulkan calls and covered by unit tests.
/// A key as a request holds it, looked up without copying its bindings.
struct FrameCacheKeyView {
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    std::span<const FrameCacheBinding> bindings;

    [[nodiscard]] bool operator==(const FrameCacheKeyView& o) const noexcept;
};

struct FrameCacheKey {
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    std::vector<FrameCacheBinding> bindings;

    [[nodiscard]] FrameCacheKeyView view() const noexcept { return {layout, bindings}; }
    bool operator==(const FrameCacheKey& o) const noexcept { return view() == o.view(); }
};

/// Hashes a key or a request's view of one alike, so a lookup copies nothing.
struct FrameCacheKeyHash {
    using is_transparent = void;
    size_t operator()(const FrameCacheKeyView& k) const noexcept;
    size_t operator()(const FrameCacheKey& k) const noexcept { return (*this)(k.view()); }
};

/// Compares keys and views of keys alike.
struct FrameCacheKeyEqual {
    using is_transparent = void;
    static FrameCacheKeyView as_view(const FrameCacheKeyView& k) noexcept { return k; }
    static FrameCacheKeyView as_view(const FrameCacheKey& k) noexcept { return k.view(); }
    template <typename A, typename B>
    bool operator()(const A& a, const B& b) const noexcept {
        return as_view(a) == as_view(b);
    }
};

/// Per-frame descriptor set cache. Passes call acquire() during record()
/// to bind views into sets without owning pools or caching sets themselves.
///
/// Lifecycle: begin_frame(i) resets pool[i]. All sets returned from
/// acquire() while the cache is on frame i are valid until the next
/// begin_frame(i) — i.e. one full roundtrip through MAX_FRAMES_IN_FLIGHT.
///
/// Within a single frame, two acquire() calls with identical bindings
/// return the same VkDescriptorSet; the cache dedups on (layout, bindings).
///
/// Any thread may acquire, under a lock: passes recorded in parallel bind
/// through the same cache.
class FrameDescriptorCache {
public:
    FrameDescriptorCache() = default;
    ~FrameDescriptorCache() = default;

    FrameDescriptorCache(const FrameDescriptorCache&) = delete;
    FrameDescriptorCache& operator=(const FrameDescriptorCache&) = delete;
    FrameDescriptorCache(FrameDescriptorCache&&) = delete;
    FrameDescriptorCache& operator=(FrameDescriptorCache&&) = delete;

    void create(VkDevice device, uint32_t frames_in_flight,
                const FrameCachePoolBudget& budget);
    void destroy();

    /// Call at the start of every frame with the current frame slot.
    /// Resets that slot's pool — any sets previously returned for slot i
    /// are invalid after this call.
    void begin_frame(uint32_t frame_index);

    /// Return a descriptor set for (layout, bindings) for the current frame.
    /// Duplicate calls with identical bindings within the same frame return
    /// the cached set.
    [[nodiscard]] VkDescriptorSet acquire(VkDescriptorSetLayout layout,
                                          std::span<const FrameCacheBinding> bindings);

    // ── Stats (rendered in the stats panel in C2a.2) ───────────────────
    [[nodiscard]] uint32_t sets_acquired_last_frame() const noexcept { return stats_.sets_last_frame; }
    [[nodiscard]] uint32_t cache_hits_last_frame() const noexcept { return stats_.hits_last_frame; }
    [[nodiscard]] uint32_t overflow_pools_last_frame() const noexcept { return stats_.overflows_last_frame; }

private:
    struct FrameSlot {
        VkDescriptorPool primary{VK_NULL_HANDLE};
        std::vector<VkDescriptorPool> overflow;
        std::unordered_map<FrameCacheKey, VkDescriptorSet, FrameCacheKeyHash, FrameCacheKeyEqual>
            cache;
    };

    struct Stats {
        uint32_t sets_this_frame{0};
        uint32_t hits_this_frame{0};
        uint32_t overflows_this_frame{0};
        uint32_t sets_last_frame{0};
        uint32_t hits_last_frame{0};
        uint32_t overflows_last_frame{0};
    };

    [[nodiscard]] VkDescriptorPool allocate_overflow_pool();
    [[nodiscard]] VkDescriptorSet try_allocate(VkDescriptorPool pool,
                                               VkDescriptorSetLayout layout);
    void write_bindings(VkDescriptorSet set,
                        std::span<const FrameCacheBinding> bindings);

    VkDevice device_{VK_NULL_HANDLE};
    FrameCachePoolBudget budget_{};
    std::vector<FrameSlot> slots_;
    uint32_t current_frame_{0};
    Stats stats_{};
    /// Guards the slots, their pools and the stats while sets are acquired.
    std::mutex mutex_;
};

} // namespace fjell
