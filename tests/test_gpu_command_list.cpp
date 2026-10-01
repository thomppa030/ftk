#include "ftk/gpu/command_list.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace fjell::gpu;

TEST_CASE("Push data must cover what the shaders read", "[gpu][commands]") {
    ShaderLayout layout;
    layout.push_size = 8;

    auto exact = push_size(layout, 8);
    REQUIRE(exact.has_value());
    CHECK(*exact == 8);

    // A C++ struct padded past the shader's block pushes only what is read.
    auto padded = push_size(layout, 16);
    REQUIRE(padded.has_value());
    CHECK(*padded == 8);

    auto short_value = push_size(layout, 4);
    REQUIRE_FALSE(short_value.has_value());
    CHECK(short_value.error().find("read 8 bytes") != std::string::npos);
    CHECK(short_value.error().find("given 4") != std::string::npos);
}

TEST_CASE("Push data is refused by shaders that take none", "[gpu][commands]") {
    const ShaderLayout layout;
    auto refused = push_size(layout, 4);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("no push data") != std::string::npos);
}

TEST_CASE("The sets shaders declare are one bit each", "[gpu][commands]") {
    ShaderLayout layout;
    for (uint32_t set : {0U, 2U, 2U, 5U}) {
        ShaderBinding binding;
        binding.set = set;
        layout.bindings.push_back(binding);
    }
    CHECK(declared_sets(layout) == 0b100101U);
    CHECK(declared_sets(ShaderLayout{}) == 0U);
}
