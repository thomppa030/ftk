#include "renderer/resource_handle.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell;

TEST_CASE("Default handle is invalid", "[handle]") {
    ModelHandle h{};
    REQUIRE_FALSE(h.valid());
    REQUIRE(h.id == 0);
}

TEST_CASE("Make produces valid handle", "[handle]") {
    auto h = ModelHandle::make(1, 1);
    REQUIRE(h.valid());
}

TEST_CASE("Index round-trips", "[handle]") {
    auto h = ModelHandle::make(42, 0);
    REQUIRE(h.index() == 42);
}

TEST_CASE("Generation round-trips", "[handle]") {
    auto h = ModelHandle::make(0, 7);
    REQUIRE(h.generation() == 7);
}

TEST_CASE("Index and generation round-trip together", "[handle]") {
    auto h = ModelHandle::make(12345, 99);
    REQUIRE(h.index() == 12345);
    REQUIRE(h.generation() == 99);
}

TEST_CASE("Max index (20 bits)", "[handle]") {
    auto h = ModelHandle::make(0xFFFFF, 1);
    REQUIRE(h.index() == 0xFFFFF);
    REQUIRE(h.generation() == 1);
}

TEST_CASE("Max generation (12 bits)", "[handle]") {
    auto h = ModelHandle::make(1, 0xFFF);
    REQUIRE(h.index() == 1);
    REQUIRE(h.generation() == 0xFFF);
}

TEST_CASE("Index overflow truncates to 20 bits", "[handle]") {
    auto h = ModelHandle::make(0x100000, 0); // bit 20 set
    REQUIRE(h.index() == 0);
}

TEST_CASE("Handles with same index and generation are equal", "[handle]") {
    auto a = ModelHandle::make(5, 3);
    auto b = ModelHandle::make(5, 3);
    REQUIRE(a == b);
}

TEST_CASE("Handles with different index are not equal", "[handle]") {
    auto a = ModelHandle::make(1, 3);
    auto b = ModelHandle::make(2, 3);
    REQUIRE(a != b);
}

TEST_CASE("Handles with different generation are not equal", "[handle]") {
    auto a = ModelHandle::make(1, 1);
    auto b = ModelHandle::make(1, 2);
    REQUIRE(a != b);
}

TEST_CASE("Different tag types are distinct types", "[handle]") {
    auto model = ModelHandle::make(1, 1);
    auto material = MaterialHandle::make(1, 1);
    REQUIRE(model.id == material.id); // same bits
    STATIC_REQUIRE_FALSE(std::is_same_v<ModelHandle, MaterialHandle>);
}

TEST_CASE("Zero index with non-zero generation is valid", "[handle]") {
    auto h = ModelHandle::make(0, 1);
    REQUIRE(h.valid());
    REQUIRE(h.index() == 0);
    REQUIRE(h.generation() == 1);
}
