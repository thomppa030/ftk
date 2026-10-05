#include "ftk/base/compression.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

// Bytes that compress: a pattern repeating every 64, with a little change.
std::vector<std::byte> patterned(size_t size) {
    std::vector<std::byte> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::byte>((i % 64) ^ (i / 4096));
    return bytes;
}

} // namespace

TEST_CASE("What is compressed comes back as it was", "[compression]") {
    const std::vector<std::byte> bytes = patterned(100000);
    for (const int level : {ftk::FASTEST_COMPRESSION, ftk::DEFAULT_COMPRESSION, ftk::SMALLEST_COMPRESSION}) {
        const std::vector<std::byte> frame = ftk::compress(bytes, level);
        CHECK(frame.size() < bytes.size() / 10);
        REQUIRE(ftk::decompressed_size(frame) == bytes.size());
        std::vector<std::byte> back(bytes.size());
        REQUIRE(ftk::decompress(frame, back).has_value());
        CHECK(back == bytes);
    }
}

TEST_CASE("Nothing compresses to a frame of nothing", "[compression]") {
    const std::vector<std::byte> frame = ftk::compress({});
    CHECK_FALSE(frame.empty());
    CHECK(ftk::decompressed_size(frame) == 0U);
    CHECK(ftk::decompress(frame, {}).has_value());
}

TEST_CASE("A frame decompresses only into its own size, and damage is caught", "[compression]") {
    const std::vector<std::byte> bytes = patterned(5000);
    std::vector<std::byte> frame = ftk::compress(bytes);

    std::vector<std::byte> too_small(bytes.size() - 1);
    const auto refused = ftk::decompress(frame, too_small);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == "the frame decompresses to 5000 bytes, not 4999");

    const std::vector<std::byte> not_a_frame(64, std::byte{0x2A});
    CHECK_FALSE(ftk::decompressed_size(not_a_frame).has_value());

    // A frame cut short.
    std::vector<std::byte> out(bytes.size());
    const std::vector<std::byte> cut(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(frame.size() / 2));
    CHECK_FALSE(ftk::decompress(cut, out).has_value());
}
