#include "ftk/image/ico.hpp"
#include "ftk/image/png.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

using ftk::image::Rgba8;

// The desktop tile's shape: a dark rounded square.
constexpr std::string_view TILE = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect x="2" y="2" width="96" height="96" rx="22" fill="#1e1f22"/></svg>)";

Rgba8 square(uint32_t side) {
    return {.texels = std::vector<uint8_t>(static_cast<std::size_t>(side) * side * 4, 128), .width = side, .height = side};
}

uint32_t read16(std::span<const std::byte> bytes, std::size_t at) {
    return std::to_integer<uint32_t>(bytes[at]) | (std::to_integer<uint32_t>(bytes[at + 1]) << 8);
}

uint32_t read32(std::span<const std::byte> bytes, std::size_t at) {
    return read16(bytes, at) | (read16(bytes, at + 2) << 16);
}

} // namespace

TEST_CASE("A program's icon is drawn at every size a desktop asks for, largest first", "[image][ico]") {
    const auto sizes = ftk::image::rasterize_icon(TILE);
    REQUIRE(sizes.has_value());
    REQUIRE(sizes->size() == ftk::image::ICON_SIZES.size());
    for (std::size_t i = 0; i < sizes->size(); ++i) {
        const uint32_t side = ftk::image::ICON_SIZES[i];
        CHECK((*sizes)[i].width == side);
        CHECK((*sizes)[i].height == side);
        CHECK((*sizes)[i].texels.size() == static_cast<std::size_t>(side) * side * 4);
    }
    CHECK(sizes->front().width == 256);
    CHECK(sizes->back().width == 16);
}

TEST_CASE("An icon that isn't an SVG says so", "[image][ico]") {
    const auto sizes = ftk::image::rasterize_icon("a shopping list");
    REQUIRE_FALSE(sizes.has_value());
    CHECK_FALSE(sizes.error().empty());
}

TEST_CASE("An ICO file lists its pictures, each one held as a PNG where its entry points", "[image][ico]") {
    const std::vector<Rgba8> sizes{square(256), square(32), square(16)};
    const std::vector<std::byte> ico = ftk::image::encode_ico(sizes);
    REQUIRE(ico.size() > 6 + 16 * sizes.size());

    CHECK(read16(ico, 0) == 0); // reserved
    CHECK(read16(ico, 2) == 1); // an icon
    CHECK(read16(ico, 4) == sizes.size());

    std::size_t expected_offset = 6 + 16 * sizes.size();
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const std::size_t entry = 6 + 16 * i;
        // A side of 256 doesn't fit the byte and is written as 0.
        const uint32_t side = sizes[i].width == 256 ? 0 : sizes[i].width;
        CHECK(std::to_integer<uint32_t>(ico[entry]) == side);
        CHECK(std::to_integer<uint32_t>(ico[entry + 1]) == side);
        CHECK(std::to_integer<uint32_t>(ico[entry + 2]) == 0);
        CHECK(std::to_integer<uint32_t>(ico[entry + 3]) == 0);
        CHECK(read16(ico, entry + 4) == 1);
        CHECK(read16(ico, entry + 6) == 32);
        const uint32_t size = read32(ico, entry + 8);
        const uint32_t offset = read32(ico, entry + 12);
        CHECK(offset == expected_offset);
        REQUIRE(offset + size <= ico.size());

        const auto picture = ftk::image::decode_png(std::span(ico).subspan(offset, size));
        REQUIRE(picture.has_value());
        CHECK(picture->width == sizes[i].width);
        CHECK(picture->height == sizes[i].height);
        CHECK(picture->texels == sizes[i].texels);
        expected_offset += size;
    }
    CHECK(expected_offset == ico.size());
}

TEST_CASE("Pictures an ICO file can't hold make no file", "[image][ico]") {
    CHECK(ftk::image::encode_ico({}).empty());
    CHECK(ftk::image::encode_ico(std::vector<Rgba8>{square(512)}).empty());
    CHECK(ftk::image::encode_ico(std::vector<Rgba8>{square(32), square(0)}).empty());
    const Rgba8 wide{.texels = std::vector<uint8_t>(static_cast<std::size_t>(64) * 32 * 4, 128), .width = 64, .height = 32};
    CHECK(ftk::image::encode_ico(std::vector<Rgba8>{wide}).empty());
}
