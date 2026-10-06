#include "ftk/gpu/access.hpp"
#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>

using namespace ftk::gpu;

namespace {

constexpr int ACCESS_COUNT = static_cast<int>(Access::acceleration_trace_compute) + 1;

} // namespace

TEST_CASE("Every access applies to a texture, a buffer or an acceleration structure, and to "
          "more than one only for copies, clears and build inputs",
          "[gpu][access]") {
    for (int i = 0; i < ACCESS_COUNT; ++i) {
        const auto access = static_cast<Access>(i);
        INFO("access " << i);
        const int kinds = int{applies_to_texture(access)} + int{applies_to_buffer(access)} +
                          int{applies_to_acceleration(access)};
        CHECK(kinds >= 1);
        CHECK((kinds > 1) == (access == Access::copy_src || access == Access::copy_dst ||
                              access == Access::clear || access == Access::acceleration_build_input));
    }
}

TEST_CASE("Every access has a name of its own", "[gpu][access]") {
    std::set<std::string_view> names;
    for (int i = 0; i < ACCESS_COUNT; ++i) {
        const auto access = static_cast<Access>(i);
        INFO("access " << i);
        CHECK_FALSE(access_name(access).empty());
        CHECK(names.insert(access_name(access)).second);
    }
    CHECK(access_name(Access::storage_buffer_write_compute) == "storage_buffer_write_compute");
}

TEST_CASE("A barrier refuses an access its resource has no scope for", "[gpu][access]") {
    CHECK(first_misapplied({}, applies_to_buffer) == std::nullopt);
    CHECK(first_misapplied(Access::copy_dst | Access::storage_buffer_write_compute, applies_to_buffer) ==
          std::nullopt);
    // A storage image's write on a buffer: the mistake that orders nothing.
    CHECK(first_misapplied(Access::copy_dst | Access::storage_write_compute, applies_to_buffer) ==
          Access::storage_write_compute);
    CHECK(first_misapplied(Access::uniform_read, applies_to_texture) == Access::uniform_read);
    CHECK(first_misapplied(Access::acceleration_build_input, applies_to_acceleration) == std::nullopt);

    CHECK(barrier_accesses(Access::copy_dst, Access::host_read, applies_to_buffer, "a buffer").has_value());
    const auto refused = barrier_accesses(Access::copy_dst | Access::storage_write_compute,
                                          Access::host_read, applies_to_buffer, "a buffer");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == "storage_write_compute is not an access a buffer has");
    // Either side is checked.
    CHECK_FALSE(barrier_accesses(Access::copy_dst, Access::sampled_fragment, applies_to_buffer, "a buffer")
                    .has_value());
}

TEST_CASE("Every texture access but a copy, a clear or present asks a use of the texture",
          "[gpu][access]") {
    for (int i = 0; i < ACCESS_COUNT; ++i) {
        const auto access = static_cast<Access>(i);
        INFO("access " << i);
        const bool asks_nothing = access == Access::copy_src || access == Access::copy_dst ||
                                  access == Access::clear || access == Access::present;
        CHECK(texture_use(access).empty() == (!applies_to_texture(access) || asks_nothing));
    }
    CHECK(texture_use(Access::depth_read_sampled) ==
          (TextureUse::depth_target | TextureUse::sampled));
}

TEST_CASE("A clear writes without reading what was there", "[gpu][access]") {
    CHECK(access_is_write(Access::clear));
    CHECK_FALSE(access_is_read(Access::clear));
}

TEST_CASE("Every format copied through a buffer has a texel size or a block size", "[gpu][format]") {
    for (int i = 1; i <= static_cast<int>(Format::bc7_srgb); ++i) {
        const auto format = static_cast<Format>(i);
        INFO("format " << i);
        CHECK((texel_size(format) == 0) == (format == Format::d32_float_s8_uint || is_block_compressed(format)));
        CHECK((block_size(format) != 0) == is_block_compressed(format));
        CHECK((packed_size(format, 4, 4) == 0) == (format == Format::d32_float_s8_uint));
    }
    CHECK(texel_size(Format::rgba8_unorm) == 4);
    CHECK(texel_size(Format::rgba16_float) == 8);
    CHECK(texel_size(Format::rgba32_float) == 16);
}

TEST_CASE("A pair of 32-bit whole numbers is whole numbers, eight bytes a texel", "[gpu][format]") {
    CHECK(kind(Format::rg32_uint) == FormatKind::color_uint);
    CHECK(texel_size(Format::rg32_uint) == 8);
    CHECK(packed_size(Format::rg32_uint, 3, 2) == 8 * 3 * 2);
}

TEST_CASE("A block-compressed format is sized by whole 4 by 4 blocks", "[gpu][format]") {
    CHECK(packed_size(Format::bc7_srgb, 2048, 2048) == 2048ULL * 2048);
    CHECK(packed_size(Format::bc4_unorm, 2048, 2048) == 2048ULL * 2048 / 2);
    // A mip under a block, and an edge that ends part way through one.
    CHECK(packed_size(Format::bc5_unorm, 2, 1) == 16);
    CHECK(packed_size(Format::bc7_unorm, 6, 9) == 16 * 2 * 3);
    CHECK(packed_size(Format::bc6h_ufloat, 4, 4, 3) == 16 * 3);
    CHECK(packed_size(Format::rgba8_unorm, 6, 9) == 4 * 6 * 9);
    CHECK(kind(Format::bc6h_ufloat) == FormatKind::color);
    CHECK_FALSE(is_block_compressed(Format::rgba8_srgb));
}
