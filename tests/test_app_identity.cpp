#include "ftk/app/app_identity.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("A program's icon is drawn at every size a desktop asks for, largest first", "[app][icon]") {
    constexpr const char* TILE = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect x="2" y="2" width="96" height="96" rx="22" fill="#1e1f22"/></svg>)";
    const auto sizes = ftk::draw_app_icon(TILE);
    REQUIRE(sizes.has_value());
    REQUIRE(sizes->size() == ftk::APP_ICON_SIZES.size());
    for (std::size_t i = 0; i < sizes->size(); ++i) {
        CHECK((*sizes)[i].width == ftk::APP_ICON_SIZES[i]);
        CHECK((*sizes)[i].height == ftk::APP_ICON_SIZES[i]);
        CHECK((*sizes)[i].texels.size() == static_cast<std::size_t>(ftk::APP_ICON_SIZES[i]) * ftk::APP_ICON_SIZES[i] * 4);
    }
    CHECK(sizes->front().width == 256);
    CHECK(sizes->back().width == 16);
}

TEST_CASE("An icon that isn't an SVG says so", "[app][icon]") {
    const auto sizes = ftk::draw_app_icon("a shopping list");
    REQUIRE_FALSE(sizes.has_value());
    CHECK_FALSE(sizes.error().empty());
}
