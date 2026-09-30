#include "gpu/access.hpp"
#include "gpu/command_list.hpp"
#include "gpu/format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>

using namespace fjell::gpu;

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
