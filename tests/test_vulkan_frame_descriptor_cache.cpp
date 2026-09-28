#include "gpu/vulkan/frame_descriptor_cache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace fjell;

namespace {

// Synthesize opaque handle values. We cast from reinterpret_cast<uintptr_t>
// so tests don't have to call into Vulkan; equality + hashing only read
// the handle values, not dereference them.
template <typename T>
T fake_handle(uintptr_t n) {
    return reinterpret_cast<T>(n);
}

FrameCacheBinding sampled(uint32_t binding, VkImageView view,
                          VkSampler sampler = fake_handle<VkSampler>(0x1),
                          VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    FrameCacheBinding b{};
    b.binding = binding;
    b.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.image.sampler = sampler;
    b.image.imageView = view;
    b.image.imageLayout = layout;
    return b;
}

FrameCacheBinding storage_buf(uint32_t binding, VkBuffer buf,
                              VkDeviceSize offset = 0,
                              VkDeviceSize range = VK_WHOLE_SIZE) {
    FrameCacheBinding b{};
    b.binding = binding;
    b.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b.buffer.buffer = buf;
    b.buffer.offset = offset;
    b.buffer.range = range;
    return b;
}

} // namespace

TEST_CASE("FrameCacheKey: empty keys with same layout are equal", "[frame_cache_key]") {
    FrameCacheKey a{fake_handle<VkDescriptorSetLayout>(0x10), {}};
    FrameCacheKey b{fake_handle<VkDescriptorSetLayout>(0x10), {}};
    REQUIRE(a == b);
    REQUIRE(FrameCacheKeyHash{}(a) == FrameCacheKeyHash{}(b));
}

TEST_CASE("FrameCacheKey: different layouts compare unequal", "[frame_cache_key]") {
    FrameCacheKey a{fake_handle<VkDescriptorSetLayout>(0x10), {}};
    FrameCacheKey b{fake_handle<VkDescriptorSetLayout>(0x11), {}};
    REQUIRE_FALSE(a == b);
}

TEST_CASE("FrameCacheKey: same view and sampler dedup", "[frame_cache_key]") {
    auto layout = fake_handle<VkDescriptorSetLayout>(0x10);
    auto view = fake_handle<VkImageView>(0x20);
    FrameCacheKey a{layout, {sampled(0, view)}};
    FrameCacheKey b{layout, {sampled(0, view)}};
    REQUIRE(a == b);
    REQUIRE(FrameCacheKeyHash{}(a) == FrameCacheKeyHash{}(b));
}

TEST_CASE("FrameCacheKey: different image views do not dedup", "[frame_cache_key]") {
    auto layout = fake_handle<VkDescriptorSetLayout>(0x10);
    FrameCacheKey a{layout, {sampled(0, fake_handle<VkImageView>(0x20))}};
    FrameCacheKey b{layout, {sampled(0, fake_handle<VkImageView>(0x21))}};
    REQUIRE_FALSE(a == b);
}

TEST_CASE("FrameCacheKey: layout transition (same view) breaks equality",
          "[frame_cache_key]") {
    // This is the load-bearing case: the same VkImageView sampled at
    // SHADER_READ_ONLY_OPTIMAL vs GENERAL must NOT dedup, because the
    // descriptor's imageLayout field is semantic.
    auto layout = fake_handle<VkDescriptorSetLayout>(0x10);
    auto view = fake_handle<VkImageView>(0x20);
    auto sampler = fake_handle<VkSampler>(0x1);
    FrameCacheKey a{layout, {sampled(0, view, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)}};
    FrameCacheKey b{layout, {sampled(0, view, sampler, VK_IMAGE_LAYOUT_GENERAL)}};
    REQUIRE_FALSE(a == b);
}

TEST_CASE("FrameCacheKey: different samplers on same view do not dedup",
          "[frame_cache_key]") {
    auto layout = fake_handle<VkDescriptorSetLayout>(0x10);
    auto view = fake_handle<VkImageView>(0x20);
    FrameCacheKey a{layout, {sampled(0, view, fake_handle<VkSampler>(0x1))}};
    FrameCacheKey b{layout, {sampled(0, view, fake_handle<VkSampler>(0x2))}};
    REQUIRE_FALSE(a == b);
}

TEST_CASE("FrameCacheKey: binding index matters", "[frame_cache_key]") {
    auto layout = fake_handle<VkDescriptorSetLayout>(0x10);
    auto view = fake_handle<VkImageView>(0x20);
    FrameCacheKey a{layout, {sampled(0, view)}};
    FrameCacheKey b{layout, {sampled(1, view)}};
    REQUIRE_FALSE(a == b);
}

TEST_CASE("FrameCacheKey: array element matters", "[frame_cache_key]") {
    auto layout = fake_handle<VkDescriptorSetLayout>(0x100);
    auto view = fake_handle<VkImageView>(0x200);
    FrameCacheKey a{layout, {sampled(0, view)}};
    FrameCacheBinding second = sampled(0, view);
    second.element = 1;
    FrameCacheKey b{layout, {second}};
    CHECK_FALSE(a == b);
}

TEST_CASE("FrameCacheKey: buffer binding equality is byte-exact",
          "[frame_cache_key]") {
    auto layout = fake_handle<VkDescriptorSetLayout>(0x10);
    auto buf = fake_handle<VkBuffer>(0x30);
    FrameCacheKey a{layout, {storage_buf(0, buf, 0, 256)}};
    FrameCacheKey b{layout, {storage_buf(0, buf, 0, 256)}};
    FrameCacheKey c{layout, {storage_buf(0, buf, 0, 512)}};   // different range
    FrameCacheKey d{layout, {storage_buf(0, buf, 128, 256)}}; // different offset
    FrameCacheKey e{layout, {storage_buf(1, buf, 0, 256)}};   // different binding
    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
    REQUIRE_FALSE(a == d);
    REQUIRE_FALSE(a == e);
}

TEST_CASE("FrameCacheKey: mixed image+buffer binding lists", "[frame_cache_key]") {
    auto layout = fake_handle<VkDescriptorSetLayout>(0x10);
    auto view = fake_handle<VkImageView>(0x20);
    auto buf = fake_handle<VkBuffer>(0x30);
    FrameCacheKey a{layout, {sampled(0, view), storage_buf(1, buf)}};
    FrameCacheKey b{layout, {sampled(0, view), storage_buf(1, buf)}};
    FrameCacheKey c{layout, {storage_buf(1, buf), sampled(0, view)}}; // order matters
    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
}

TEST_CASE("FrameCacheKey: hash collisions on layout-only difference are unlikely",
          "[frame_cache_key]") {
    // Regression guard: if someone changes the hash to drop layout, many
    // layouts would collide. Sample a few and require uniqueness.
    FrameCacheKeyHash h{};
    std::unordered_set<size_t> hashes;
    for (uintptr_t i = 1; i <= 32; ++i) {
        FrameCacheKey k{fake_handle<VkDescriptorSetLayout>(i), {}};
        hashes.insert(h(k));
    }
    // 32 distinct layout handles should produce at least 30 distinct
    // hashes — a birthday-paradox miss or two is fine, but total collapse
    // would indicate a broken hash.
    REQUIRE(hashes.size() >= 30);
}

TEST_CASE("FrameCacheKey: hash differentiates image layouts", "[frame_cache_key]") {
    auto layout = fake_handle<VkDescriptorSetLayout>(0x10);
    auto view = fake_handle<VkImageView>(0x20);
    auto sampler = fake_handle<VkSampler>(0x1);
    FrameCacheKey read_only{layout, {sampled(0, view, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)}};
    FrameCacheKey general{layout, {sampled(0, view, sampler, VK_IMAGE_LAYOUT_GENERAL)}};
    REQUIRE(FrameCacheKeyHash{}(read_only) != FrameCacheKeyHash{}(general));
}

TEST_CASE("FrameCacheKey: a request's view finds the key it matches", "[frame_cache_key]") {
    auto layout = fake_handle<VkDescriptorSetLayout>(0x300);
    const FrameCacheKey key{layout, {sampled(0, fake_handle<VkImageView>(0x400)),
                                     storage_buf(1, fake_handle<VkBuffer>(0x500), 64, 128)}};
    const std::vector<FrameCacheBinding> request = key.bindings;
    const FrameCacheKeyView view{layout, request};

    CHECK(FrameCacheKeyEqual{}(key, view));
    CHECK(FrameCacheKeyEqual{}(view, key));
    CHECK(FrameCacheKeyHash{}(key) == FrameCacheKeyHash{}(view));

    std::unordered_map<FrameCacheKey, int, FrameCacheKeyHash, FrameCacheKeyEqual> cache;
    cache.emplace(key, 7);
    const auto found = cache.find(view);
    REQUIRE(found != cache.end());
    CHECK(found->second == 7);

    std::vector<FrameCacheBinding> other = request;
    other[1].buffer.offset = 0;
    CHECK(cache.find(FrameCacheKeyView{layout, other}) == cache.end());
}
