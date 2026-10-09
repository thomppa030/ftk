#include "ftk/math/color_gradient.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

using ftk::ColorGradient;
using ftk::GradientBlend;
using Catch::Approx;

namespace {

bool close(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(a - b) < 1e-4f;
}

ColorGradient black_to_white() {
    ColorGradient g;
    g.keys = {{0.25f, glm::vec3(0.0f)}, {0.75f, glm::vec3(1.0f)}};
    return g;
}

} // namespace

TEST_CASE("A colour gradient blends in linear light and can wrap", "[color_gradient]") {
    const ColorGradient g = black_to_white();

    REQUIRE(close(g.evaluate(0.5f), glm::vec3(0.5f)));
    REQUIRE(close(g.evaluate(0.0f), glm::vec3(0.0f)));   // holds the first stop
    REQUIRE(close(g.evaluate(1.0f), glm::vec3(1.0f)));   // holds the last stop

    // Wrapped, the gap from 0.75 back around to 0.25 is one half-period blend.
    REQUIRE(close(g.evaluate(0.0f, true), glm::vec3(0.5f)));
    REQUIRE(close(g.evaluate(0.875f, true), glm::vec3(0.75f)));
    REQUIRE(close(g.evaluate(0.125f, true), glm::vec3(0.25f)));

    // Stops sort themselves and survive the sRGB file round trip.
    ColorGradient unsorted;
    unsorted.keys = {{0.8f, glm::vec3(1.0f, 0.0f, 0.0f)}, {0.2f, glm::vec3(0.0f, 0.0f, 1.0f)}};
    nlohmann::json j;
    unsorted.to_json(j);
    ColorGradient back;
    back.from_json(j);
    REQUIRE(back.keys.size() == 2);
    REQUIRE(back.keys[0].time == Approx(0.2f));
    REQUIRE(close(back.keys[0].color, glm::vec3(0.0f, 0.0f, 1.0f)));
    REQUIRE(close(back.keys[1].color, glm::vec3(1.0f, 0.0f, 0.0f)));
}

TEST_CASE("A colour gradient eases between stops, or holds each", "[color_gradient]") {
    ColorGradient g = black_to_white();

    g.blend = GradientBlend::smooth;
    // A quarter of the way: smoothstep's 0.15625, not 0.25.
    REQUIRE(close(g.evaluate(0.375f), glm::vec3(0.15625f)));
    REQUIRE(close(g.evaluate(0.5f), glm::vec3(0.5f)));
    REQUIRE(close(g.evaluate(0.75f), glm::vec3(1.0f)));

    g.blend = GradientBlend::constant;
    REQUIRE(close(g.evaluate(0.7f), glm::vec3(0.0f)));   // the first stop's colour until the next
    REQUIRE(close(g.evaluate(0.75f), glm::vec3(1.0f)));
    REQUIRE(close(g.evaluate(0.0f, true), glm::vec3(1.0f)));   // across the seam, the last stop's
}

TEST_CASE("A linear gradient keeps the file shape of old; another names its blend", "[color_gradient]") {
    ColorGradient linear = black_to_white();
    nlohmann::json j;
    linear.to_json(j);
    REQUIRE(j.is_array());

    ColorGradient smooth = black_to_white();
    smooth.blend = GradientBlend::smooth;
    nlohmann::json s;
    smooth.to_json(s);
    REQUIRE(s.is_object());
    REQUIRE(s["blend"] == "smooth");

    ColorGradient back;
    back.from_json(s);
    REQUIRE(back == smooth);

    // A file written before blends reads as linear.
    back.from_json(j);
    REQUIRE(back.blend == GradientBlend::linear);
    REQUIRE(back.keys.size() == 2);
}
