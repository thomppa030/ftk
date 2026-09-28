#include "gpu/access.hpp"
#include "gpu/format.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell::gpu;

namespace {

constexpr int ACCESS_COUNT = static_cast<int>(Access::clear) + 1;

} // namespace

TEST_CASE("Every access applies to a texture, a buffer, or to both only for copies and clears",
          "[gpu][access]") {
    for (int i = 0; i < ACCESS_COUNT; ++i) {
        const auto access = static_cast<Access>(i);
        INFO("access " << i);
        CHECK((applies_to_texture(access) || applies_to_buffer(access)));
        const bool both = applies_to_texture(access) && applies_to_buffer(access);
        CHECK(both == (access == Access::copy_src || access == Access::copy_dst ||
                       access == Access::clear));
    }
}

TEST_CASE("Every texture access but a copy or a clear asks a use of the texture",
          "[gpu][access]") {
    for (int i = 0; i < ACCESS_COUNT; ++i) {
        const auto access = static_cast<Access>(i);
        INFO("access " << i);
        const bool copy_or_clear =
            access == Access::copy_src || access == Access::copy_dst || access == Access::clear;
        CHECK(texture_use(access).empty() == (!applies_to_texture(access) || copy_or_clear));
    }
    CHECK(texture_use(Access::depth_read_sampled) ==
          (TextureUse::depth_target | TextureUse::sampled));
}

TEST_CASE("A clear writes without reading what was there", "[gpu][access]") {
    CHECK(access_is_write(Access::clear));
    CHECK_FALSE(access_is_read(Access::clear));
}

TEST_CASE("Every format copied through a buffer has a texel size", "[gpu][format]") {
    for (int i = 1; i <= static_cast<int>(Format::d32_float_s8_uint); ++i) {
        const auto format = static_cast<Format>(i);
        INFO("format " << i);
        CHECK((texel_size(format) == 0) == (format == Format::d32_float_s8_uint));
    }
    CHECK(texel_size(Format::rgba8_unorm) == 4);
    CHECK(texel_size(Format::rgba16_float) == 8);
    CHECK(texel_size(Format::rgba32_float) == 16);
}
