#include "ftk/base/hash.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

TEST_CASE("FNV-1a gives the published values", "[hash]") {
    // From the reference test vectors of the FNV hash's authors.
    STATIC_REQUIRE(ftk::fnv1a64("") == 0xcbf29ce484222325ULL);
    STATIC_REQUIRE(ftk::fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
    STATIC_REQUIRE(ftk::fnv1a64("foobar") == 0x85944171f73967e8ULL);
}

TEST_CASE("Hashing pieces in turn is hashing them joined", "[hash]") {
    CHECK(ftk::fnv1a64("bar", ftk::fnv1a64("foo")) == ftk::fnv1a64("foobar"));
    const std::array<std::byte, 3> bytes{std::byte{'f'}, std::byte{'o'}, std::byte{'o'}};
    CHECK(ftk::fnv1a64(bytes) == ftk::fnv1a64("foo"));
}
