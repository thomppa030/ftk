#include "renderer/resources/fjterrain.hpp"
#include "renderer/resources/water_shore.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
