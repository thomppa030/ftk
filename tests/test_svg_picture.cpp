#include "ftk/app/picture.hpp"
#include "ftk/platform/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Twice as wide as it is tall.
constexpr const char* WIDE = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 10">
<rect width="20" height="10" fill="#d4a054"/></svg>)";

} // namespace

TEST_CASE("An SVG picture is as wide as its proportions say", "[app][svg]") {
    const auto picture = ftk::SvgPicture::from_text(WIDE, "wide");
    REQUIRE(picture.has_value());
    CHECK(picture->size(30.0f).x == 60.0f);
    CHECK(picture->size(30.0f).y == 30.0f);
}

TEST_CASE("An SVG picture covers whole pixels at the display's scale", "[app][svg]") {
    const auto picture = ftk::SvgPicture::from_text(WIDE, "wide");
    REQUIRE(picture.has_value());
    CHECK(picture->pixels(30.0f, 1.0f) == glm::uvec2{60, 30});
    // 92 units on a display of 1.25 pixels a unit are 115 pixels tall.
    CHECK(picture->pixels(92.0f, 1.25f) == glm::uvec2{230, 115});
    // However small, a pixel tall, and as wide as that makes it.
    CHECK(picture->pixels(0.1f, 1.0f) == glm::uvec2{2, 1});

    // A narrow picture keeps a pixel's width.
    const auto narrow = ftk::SvgPicture::from_text(
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1 10"><rect width="1" height="10"/></svg>)", "narrow");
    REQUIRE(narrow.has_value());
    CHECK(narrow->pixels(4.0f, 1.0f) == glm::uvec2{1, 4});
}

TEST_CASE("An SVG picture reads its file", "[app][svg]") {
    const fs::path folder = fs::temp_directory_path() / ("ftk_svg_picture_" + std::to_string(ftk::platform::process_id()));
    fs::create_directories(folder);
    const fs::path path = folder / "wide.svg";
    std::ofstream(path) << WIDE;

    const auto picture = ftk::SvgPicture::load(path);
    REQUIRE(picture.has_value());
    CHECK(picture->size(10.0f).x == 20.0f);

    const auto missing = ftk::SvgPicture::load(folder / "missing.svg");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("missing.svg") != std::string::npos);
    fs::remove_all(folder);
}

TEST_CASE("What isn't an SVG with a size says so, by its name", "[app][svg]") {
    const auto not_svg = ftk::SvgPicture::from_text("a shopping list", "list.svg");
    REQUIRE_FALSE(not_svg.has_value());
    CHECK(not_svg.error().starts_with("list.svg: "));
}
