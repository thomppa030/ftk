#include "renderer/resources/fjterrain.hpp"
#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_shore.hpp"
#include "renderer/resources/water_surface.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

using namespace fjell;
using Catch::Approx;

namespace {

// A terrain whose normalized height is `h(i, j)` at sample (i, j).
template <typename Fn>
std::shared_ptr<FjTerrainData> terrain(std::uint32_t samples, float world_size,
                                       float height_scale, float height_offset, Fn&& h) {
    auto data = std::make_shared<FjTerrainData>();
    data->header.sample_count = samples;
    data->header.world_size = world_size;
    data->header.height_scale = height_scale;
    data->header.height_offset = height_offset;
    data->heights.resize(static_cast<std::size_t>(samples) * samples);
    for (std::uint32_t j = 0; j < samples; ++j) {
        for (std::uint32_t i = 0; i < samples; ++i) {
            data->heights[(static_cast<std::size_t>(j) * samples) + i] = h(i, j);
        }
    }
    return data;
}

// Green's law the long way, in doubles, as a check on the fitted series:
// solve the dispersion relation x·tanh(x) = μ for x = k·d by Newton, then
// Ks = sqrt(cg_deep / cg) = sqrt(x / (2·n·μ)), n = (1 + 2x / sinh(2x)) / 2.
double solved_shoaling(double mu) {
    double x = mu < 1.0 ? std::sqrt(mu) : mu;
    for (int i = 0; i < 60; ++i) {
        const double t = std::tanh(x);
        x -= ((x * t) - mu) / (t + (x * (1.0 - (t * t))));
    }
    const double n = 0.5 * (1.0 + (2.0 * x / std::sinh(2.0 * x)));
    return std::sqrt(x / (2.0 * n * mu));
}

} // namespace

// A plane is the one surface bilinear filtering reproduces exactly, so the
// depth anywhere on it is known from the plane's own equation: the answer
// comes from the arithmetic of the setup, not from the lookup being tested.
TEST_CASE("The still depth over a sloping bed is the plane's", "[water_shore]") {
    // 65 samples over 128 m, so 2 m apart; the bed rises 0.004 of its 50 m
    // scale per sample east and 0.002 per sample north, from an offset of -20.
    constexpr float SCALE = 50.0f;
    constexpr float OFFSET = -20.0f;
    auto data = terrain(65, 128.0f, SCALE, OFFSET, [](std::uint32_t i, std::uint32_t j) {
        return 0.1f + (0.004f * static_cast<float>(i)) + (0.002f * static_cast<float>(j));
    });

    water::ShoreBed bed;
    bed.terrain = data;
    bed.min_corner = {-64.0f, 100.0f};
    bed.ground_y = 3.0f;
    bed.surface_y = 1.5f;

    // Metres east and north of the min corner, and the plane there.
    const auto expected_depth = [&](glm::vec2 field) {
        const float normalized = 0.1f + (0.004f * field.x / 2.0f) + (0.002f * field.y / 2.0f);
        return bed.surface_y - (bed.ground_y + OFFSET + (SCALE * normalized));
    };
    // d(depth)/dx: the bed rises 0.004 * 50 m every 2 m, so the depth falls
    // 0.1 m per metre east, and 0.05 per metre north.
    const glm::vec2 expected_gradient{-0.1f, -0.05f};

    for (glm::vec2 field : {glm::vec2{0.0f, 0.0f}, glm::vec2{1.0f, 0.5f}, glm::vec2{37.3f, 90.9f},
                            glm::vec2{127.0f, 3.1f}, glm::vec2{128.0f, 128.0f}}) {
        const auto depth = water::still_depth(bed, bed.min_corner + field);
        REQUIRE(depth.has_value());
        CHECK(depth->depth == Approx(expected_depth(field)).margin(1e-4));
        CHECK(depth->gradient.x == Approx(expected_gradient.x).margin(1e-4));
        CHECK(depth->gradient.y == Approx(expected_gradient.y).margin(1e-4));
    }
}

TEST_CASE("A bump in the bed reads as the bilinear tent over it", "[water_shore]") {
    // One raised sample in a flat bed. Bilinear filtering makes a four-cell
    // tent of it: full height on the sample, half at a cell's width from it
    // along an axis, a quarter at the diagonal cell centre.
    auto data = terrain(5, 4.0f, 1.0f, 0.0f, [](std::uint32_t i, std::uint32_t j) {
        return (i == 2 && j == 2) ? 1.0f : 0.0f;
    });
    water::ShoreBed bed;
    bed.terrain = data;
    bed.surface_y = 2.0f;

    CHECK(water::still_depth(bed, {2.0f, 2.0f})->depth == Approx(1.0f));
    CHECK(water::still_depth(bed, {2.5f, 2.0f})->depth == Approx(1.5f));
    CHECK(water::still_depth(bed, {2.5f, 2.5f})->depth == Approx(1.75f));
    CHECK(water::still_depth(bed, {0.5f, 0.5f})->depth == Approx(2.0f));

    // In the cell south-west of the bump the tent is tx * tz, measured from
    // the cell's corner at (1, 1), so its slope is (tz, tx): at (1.5, 1.75)
    // the bed rises 0.75 per metre east and 0.5 north, and the depth falls by
    // as much. Not on a sample line, where the tent has a ridge and no one
    // slope.
    const auto inside = water::still_depth(bed, {1.5f, 1.75f});
    CHECK(inside->gradient.x == Approx(-0.75f));
    CHECK(inside->gradient.y == Approx(-0.5f));
}

TEST_CASE("Off the terrain there is no bed", "[water_shore]") {
    auto data = terrain(3, 10.0f, 1.0f, 0.0f, [](std::uint32_t, std::uint32_t) { return 0.0f; });
    water::ShoreBed bed;
    bed.terrain = data;
    bed.min_corner = {5.0f, 5.0f};

    CHECK(water::still_depth(bed, {5.0f, 5.0f}).has_value());
    CHECK(water::still_depth(bed, {15.0f, 15.0f}).has_value());
    CHECK_FALSE(water::still_depth(bed, {4.99f, 10.0f}).has_value());
    CHECK_FALSE(water::still_depth(bed, {10.0f, 15.01f}).has_value());
    CHECK_FALSE(water::still_depth(bed, {-1000.0f, 0.0f}).has_value());

    // And no terrain at all is no bed anywhere.
    CHECK_FALSE(water::still_depth(water::ShoreBed{}, {0.0f, 0.0f}).has_value());
}

TEST_CASE("The shoaling series is Green's law solved", "[water_shore]") {
    // Against the dispersion relation solved directly, across the range from
    // a few millimetres of water to the edge of deep water.
    for (double mu = 1e-3; mu < water::SHOALING_DEEP_MU; mu *= 1.05) {
        const water::Shoaling s = water::shoaling_at(static_cast<float>(mu));
        const double solved = solved_shoaling(mu);
        CHECK(s.coefficient == Approx(solved).epsilon(2e-4));

        const double h = mu * 1e-5;
        const double slope = (solved_shoaling(mu + h) - solved_shoaling(mu - h)) / (2.0 * h);
        CHECK(s.slope == Approx(slope).margin((0.03 * std::abs(slope)) + (1e-3 * solved)));
    }

    // The published shape: a dip to about 0.91 near μ = 1 before the rise.
    CHECK(water::shoaling_at(1.0f).coefficient == Approx(0.913).margin(1e-3));

    // And deep water is untouched, with no step where the series hands over.
    CHECK(water::shoaling_at(water::SHOALING_DEEP_MU).coefficient == 1.0f);
    CHECK(water::shoaling_at(water::SHOALING_DEEP_MU).slope == 0.0f);
    CHECK(water::shoaling_at(water::SHOALING_DEEP_MU - 1e-4f).coefficient
          == Approx(1.0).margin(2e-5));
}

TEST_CASE("The coastal sea shoals and breaks as the plan worked it out", "[water_shore]") {
    // coastal.fjwater: wind 9 m/s over 60 km, raised 2.9 times. The heights
    // below were worked out for the plan by solving the dispersion relation
    // at each depth and capping at 0.39 of the depth, outside this code.
    FftContributor sea;
    sea.wind_speed = 9.0f;
    sea.fetch = 60000.0f;
    sea.amplitude_scale = 2.9f;
    sea.resolution = 8;    // the drawing's size does not change what it shoals by
    sea.patch_size = 64.0f;
    const auto drawn = water::draw_simulated_sea(sea);

    // Its peak is a 40.2 m wave and its significant amplitude 2.12 m.
    CHECK(drawn.shoaling.k == Approx(2.0 * 3.14159265 / 40.2).epsilon(2e-3));
    CHECK(drawn.shoaling.amplitude == Approx(2.12).epsilon(5e-3));

    struct Row {
        float depth;
        float amplitude;
    };
    for (const Row row : {Row{10.0f, 1.98f}, Row{6.0f, 1.94f}, Row{4.0f, 1.56f},
                          Row{2.0f, 0.78f}, Row{1.0f, 0.39f}, Row{0.5f, 0.20f}}) {
        const auto effect = water::shore_effect({row.depth, {0.0f, 0.0f}}, {}, drawn.shoaling);
        REQUIRE(effect.has_value());
        CHECK(effect->simulated * drawn.shoaling.amplitude == Approx(row.amplitude).margin(0.01));
    }
}

TEST_CASE("The shore leaves deep water and a flat sea alone", "[water_shore]") {
    const water::ShoalingSum swell{2.0f * 3.14159265f / 100.0f, 1.0f};   // a 100 m wave
    const water::ShoalingSum chop{2.0f * 3.14159265f / 10.0f, 0.3f};     // a 10 m one

    // Deep for the longest wave present is deep for the sea.
    CHECK_FALSE(water::shore_effect({120.0f, {}}, swell, chop).has_value());
    CHECK_FALSE(water::shore_effect({12.0f, {}}, {}, chop).has_value());
    // A sea with nothing in it has nothing to shoal, however shallow.
    CHECK_FALSE(water::shore_effect({0.5f, {}}, {}, {}).has_value());

    // Shallow for the swell, still deep for the chop: the chop does not grow
    // by any coefficient of its own, only shares what breaking keeps, which
    // at this depth is all of it.
    const auto mixed = water::shore_effect({12.0f, {}}, swell, chop);
    REQUIRE(mixed.has_value());
    CHECK(mixed->simulated == 1.0f);
    CHECK(mixed->gerstner != 1.0f);
    CHECK(mixed->breaking == 0.0f);
}

TEST_CASE("There is no sea on dry ground", "[water_shore]") {
    const water::ShoalingSum sea{0.2f, 1.0f};
    for (const float depth : {0.0f, -0.1f, -30.0f}) {
        const auto effect = water::shore_effect({depth, {0.3f, 0.0f}}, sea, sea);
        REQUIRE(effect.has_value());
        CHECK(effect->gerstner == 0.0f);
        CHECK(effect->simulated == 0.0f);
        CHECK(effect->breaking == 1.0f);
    }
}

TEST_CASE("The shore factor's gradient is its change across the point", "[water_shore]") {
    // Against the factor itself taken a little deeper and a little shallower:
    // a slope the code states, checked by values it computes some other way.
    const water::ShoalingSum swell{2.0f * 3.14159265f / 60.0f, 1.0f};
    const water::ShoalingSum sea{2.0f * 3.14159265f / 30.0f, 0.8f};
    const glm::vec2 slope{0.02f, -0.05f};   // the depth's own change per metre

    // From shoaling alone to deep in the breaking zone.
    for (const float depth : {20.0f, 8.0f, 5.0f, 2.5f, 1.0f, 0.3f}) {
        const auto at = water::shore_effect({depth, slope}, swell, sea);
        REQUIRE(at.has_value());
        const float h = depth * 1e-3f;
        const auto deeper = water::shore_effect({depth + h, slope}, swell, sea);
        const auto shallower = water::shore_effect({depth - h, slope}, swell, sea);
        const float g_rate = (deeper->gerstner - shallower->gerstner) / (2.0f * h);
        const float s_rate = (deeper->simulated - shallower->simulated) / (2.0f * h);
        CHECK(at->gerstner_gradient.x == Approx(g_rate * slope.x).margin(1e-4));
        CHECK(at->gerstner_gradient.y == Approx(g_rate * slope.y).margin(1e-4));
        CHECK(at->simulated_gradient.x == Approx(s_rate * slope.x).margin(1e-4));
        CHECK(at->simulated_gradient.y == Approx(s_rate * slope.y).margin(1e-4));
    }
}
