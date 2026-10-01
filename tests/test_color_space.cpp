#include "ftk/math/color_space.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace fjell::color_space;

TEST_CASE("Black and white are fixed points of the sRGB curve", "[color_space]") {
    REQUIRE(srgb_to_linear(0.0f) == 0.0f);
    REQUIRE(srgb_to_linear(1.0f) == Catch::Approx(1.0f));
    REQUIRE(linear_to_srgb(0.0f) == 0.0f);
    REQUIRE(linear_to_srgb(1.0f) == Catch::Approx(1.0f));
}

TEST_CASE("Mid grey decodes to roughly a fifth of linear light", "[color_space]") {
    REQUIRE(srgb_to_linear(0.5f) == Catch::Approx(0.2140f).margin(0.0005f));
    REQUIRE(linear_to_srgb(0.2140f) == Catch::Approx(0.5f).margin(0.0005f));
    // The low end is the linear toe, not the power curve.
    REQUIRE(srgb_to_linear(0.04f) == Catch::Approx(0.04f / 12.92f));
}

TEST_CASE("Conversion round-trips within float noise", "[color_space]") {
    for (float s : {0.01f, 0.1f, 0.25f, 0.436f, 0.7f, 0.95f, 1.5f, 2.5f}) {
        REQUIRE(linear_to_srgb(srgb_to_linear(s)) == Catch::Approx(s).epsilon(1e-5f));
    }
}

TEST_CASE("Vector overloads convert RGB and leave alpha alone", "[color_space]") {
    glm::vec4 c = srgb_to_linear(glm::vec4{0.5f, 1.0f, 0.0f, 0.6f});
    REQUIRE(c.r == Catch::Approx(0.2140f).margin(0.0005f));
    REQUIRE(c.g == Catch::Approx(1.0f));
    REQUIRE(c.b == 0.0f);
    REQUIRE(c.a == 0.6f);

    glm::vec3 back = linear_to_srgb(srgb_to_linear(glm::vec3{0.3f, 0.6f, 0.9f}));
    REQUIRE(back.r == Catch::Approx(0.3f).epsilon(1e-5f));
    REQUIRE(back.g == Catch::Approx(0.6f).epsilon(1e-5f));
    REQUIRE(back.b == Catch::Approx(0.9f).epsilon(1e-5f));
}
