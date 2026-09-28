#include "gpu/vulkan/translate.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

using namespace fjell::gpu;

namespace {

// Every format the enum declares, found without a second list to keep in
// step: to_vk() names each one (its switch has no default, so a format left
// out fails the build) and falls through to VK_FORMAT_UNDEFINED past the
// last, which is where the walk stops.
std::vector<Format> declared_formats() {
    std::vector<Format> formats;
    for (unsigned value = 1; value <= 0xFF; ++value) {
        const auto format = static_cast<Format>(value);
        if (vulkan::to_vk(format) == VK_FORMAT_UNDEFINED) break;
        formats.push_back(format);
    }
    return formats;
}

} // namespace

TEST_CASE("every format maps to a Vulkan format of its own and back", "[gpu]") {
    const auto formats = declared_formats();
    REQUIRE(formats.size() == static_cast<size_t>(Format::d32_float_s8_uint));

    std::set<VkFormat> seen;
    for (Format format : formats) {
        const VkFormat vk = vulkan::to_vk(format);
        CHECK(seen.insert(vk).second);
        CHECK(vulkan::from_vk(vk) == format);
    }
    CHECK(vulkan::to_vk(Format::undefined) == VK_FORMAT_UNDEFINED);
    CHECK(vulkan::from_vk(VK_FORMAT_UNDEFINED) == Format::undefined);
}

TEST_CASE("formats Apple GPUs lack have no name in the interface", "[gpu]") {
    CHECK(vulkan::from_vk(VK_FORMAT_D24_UNORM_S8_UINT) == Format::undefined);
    CHECK(vulkan::from_vk(VK_FORMAT_R8G8B8_UNORM) == Format::undefined);
    CHECK(vulkan::from_vk(VK_FORMAT_R32G32B32_SFLOAT) == Format::undefined);
}

TEST_CASE("a sample count maps to the same count", "[gpu]") {
    for (Samples samples : {Samples::x1, Samples::x2, Samples::x4, Samples::x8}) {
        CHECK(static_cast<uint32_t>(vulkan::to_vk(samples)) == static_cast<uint32_t>(samples));
    }
}

TEST_CASE("any texture or buffer may be copied and cleared", "[gpu]") {
    constexpr VkImageUsageFlags image_copies =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    CHECK(vulkan::to_vk(TextureUses{}) == image_copies);
    CHECK((vulkan::to_vk(TextureUses{TextureUse::sampled}) & image_copies) == image_copies);

    constexpr VkBufferUsageFlags buffer_copies =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CHECK(vulkan::to_vk(BufferUses{}) == buffer_copies);
    CHECK((vulkan::to_vk(BufferUses{BufferUse::index}) & buffer_copies) == buffer_copies);
}

TEST_CASE("each use adds usage of its own", "[gpu]") {
    const VkImageUsageFlags image_base = vulkan::to_vk(TextureUses{});
    VkImageUsageFlags image_seen = 0;
    for (TextureUse use : {TextureUse::sampled, TextureUse::storage,
                           TextureUse::color_target, TextureUse::depth_target}) {
        const VkImageUsageFlags added = vulkan::to_vk(TextureUses{use}) & ~image_base;
        CHECK(added != 0);
        CHECK((added & image_seen) == 0);
        image_seen |= added;
    }
    CHECK(vulkan::to_vk(TextureUse::storage | TextureUse::sampled) ==
          (image_base | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));

    const VkBufferUsageFlags buffer_base = vulkan::to_vk(BufferUses{});
    VkBufferUsageFlags buffer_seen = 0;
    for (BufferUse use : {BufferUse::uniform, BufferUse::storage, BufferUse::vertex,
                          BufferUse::index, BufferUse::indirect,
                          BufferUse::acceleration_input, BufferUse::device_address}) {
        const VkBufferUsageFlags added = vulkan::to_vk(BufferUses{use}) & ~buffer_base;
        CHECK(added != 0);
        CHECK((added & buffer_seen) == 0);
        buffer_seen |= added;
    }
}

TEST_CASE("a clear value is read the way the cleared format holds it", "[gpu]") {
    const Clear colour{0.25f, 0.5f, 0.75f, 1.0f};
    const VkClearValue as_float = vulkan::to_vk(colour, Format::rgba16_float);
    CHECK(as_float.color.float32[0] == 0.25f);
    CHECK(as_float.color.float32[3] == 1.0f);

    const VkClearValue as_uint = vulkan::to_vk(Clear{7.0f}, Format::r32_uint);
    CHECK(as_uint.color.uint32[0] == 7u);

    const VkClearValue as_depth = vulkan::to_vk(Clear::depth_stencil(0.5f, 3), Format::d32_float_s8_uint);
    CHECK(as_depth.depthStencil.depth == 0.5f);
    CHECK(as_depth.depthStencil.stencil == 3u);
}

TEST_CASE("a flag set holds what was put in it", "[gpu]") {
    TextureUses uses;
    CHECK(uses.empty());
    uses |= TextureUse::color_target;
    uses |= TextureUse::sampled;
    CHECK(uses.has(TextureUse::sampled));
    CHECK(uses.has(TextureUse::color_target));
    CHECK_FALSE(uses.has(TextureUse::storage));
    CHECK(uses == (TextureUse::sampled | TextureUse::color_target));

    std::vector<TextureUse> walked;
    uses.for_each([&](TextureUse use) { walked.push_back(use); });
    CHECK(walked == std::vector<TextureUse>{TextureUse::sampled, TextureUse::color_target});
}
