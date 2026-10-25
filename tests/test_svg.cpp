#include "ftk/image/svg.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace {

using ftk::image::Rgba8;

std::array<uint8_t, 4> texel(const Rgba8& picture, uint32_t x, uint32_t y) {
    const std::size_t at = (static_cast<std::size_t>(y) * picture.width + x) * 4;
    return {picture.texels[at], picture.texels[at + 1], picture.texels[at + 2], picture.texels[at + 3]};
}

// Red over the left half of a 4 × 4 viewBox; nothing over the right half.
constexpr std::string_view LEFT_HALF_RED = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 4 4">
<rect x="0" y="0" width="2" height="4" fill="#ff0000"/></svg>)";

} // namespace

TEST_CASE("An SVG is drawn into the pixels it is asked for", "[image][svg]") {
    const auto picture = ftk::image::rasterize_svg(LEFT_HALF_RED, 8, 8);
    REQUIRE(picture.has_value());
    CHECK(picture->width == 8);
    CHECK(picture->height == 8);
    CHECK(picture->texels.size() == 8 * 8 * 4);
    CHECK(texel(*picture, 1, 4) == std::array<uint8_t, 4>{255, 0, 0, 255});
    CHECK(texel(*picture, 6, 4) == std::array<uint8_t, 4>{0, 0, 0, 0});
}

TEST_CASE("An SVG's colours keep their alpha apart from them", "[image][svg]") {
    constexpr std::string_view HALF_SEEN = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 2 2">
<rect width="2" height="2" fill="#ff0000" fill-opacity="0.5"/></svg>)";
    const auto picture = ftk::image::rasterize_svg(HALF_SEEN, 4, 4);
    REQUIRE(picture.has_value());
    const auto red = texel(*picture, 2, 2);
    CHECK(red[0] == 255);
    CHECK(red[1] == 0);
    CHECK(red[3] >= 126);
    CHECK(red[3] <= 129);
}

TEST_CASE("A side given as 0 follows from the other by the SVG's proportions", "[image][svg]") {
    constexpr std::string_view WIDE = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 10">
<rect width="20" height="10" fill="#00ff00"/></svg>)";
    const auto by_width = ftk::image::rasterize_svg(WIDE, 40, 0);
    REQUIRE(by_width.has_value());
    CHECK(by_width->width == 40);
    CHECK(by_width->height == 20);

    const auto by_height = ftk::image::rasterize_svg(WIDE, 0, 30);
    REQUIRE(by_height.has_value());
    CHECK(by_height->width == 60);
    CHECK(by_height->height == 30);
}

TEST_CASE("An SVG's own size is its width and height, or its viewBox's", "[image][svg]") {
    const auto by_view = ftk::image::svg_size(LEFT_HALF_RED);
    REQUIRE(by_view.has_value());
    CHECK(by_view->width == 4.0f);
    CHECK(by_view->height == 4.0f);

    constexpr std::string_view SIZED = R"(<svg xmlns="http://www.w3.org/2000/svg" width="282" height="256"
viewBox="-130 -112 260 236"/>)";
    const auto by_size = ftk::image::svg_size(SIZED);
    REQUIRE(by_size.has_value());
    CHECK(by_size->width == 282.0f);
    CHECK(by_size->height == 256.0f);

    const auto not_svg = ftk::image::svg_size("a shopping list");
    REQUIRE_FALSE(not_svg.has_value());
    CHECK_FALSE(not_svg.error().empty());
}

TEST_CASE("Both sides given stretch the drawing to fill them", "[image][svg]") {
    const auto picture = ftk::image::rasterize_svg(LEFT_HALF_RED, 16, 4);
    REQUIRE(picture.has_value());
    CHECK(texel(*picture, 7, 2) == std::array<uint8_t, 4>{255, 0, 0, 255});
    CHECK(texel(*picture, 8, 2) == std::array<uint8_t, 4>{0, 0, 0, 0});
}

TEST_CASE("What can't be drawn says why", "[image][svg]") {
    const auto not_svg = ftk::image::rasterize_svg("a shopping list", 8, 8);
    REQUIRE_FALSE(not_svg.has_value());
    CHECK_FALSE(not_svg.error().empty());

    const auto nothing = ftk::image::rasterize_svg(LEFT_HALF_RED, 0, 0);
    REQUIRE_FALSE(nothing.has_value());
    CHECK_FALSE(nothing.error().empty());

    const auto too_large = ftk::image::rasterize_svg(LEFT_HALF_RED, ftk::image::SVG_LARGEST + 1, 8);
    REQUIRE_FALSE(too_large.has_value());
    CHECK_FALSE(too_large.error().empty());

    constexpr std::string_view TALL = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1 100"/>)";
    const auto too_tall = ftk::image::rasterize_svg(TALL, 100, 0);
    REQUIRE_FALSE(too_tall.has_value());
    CHECK_FALSE(too_tall.error().empty());
}
