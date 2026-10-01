#include "core/thread_pool.hpp"
#include "renderer/resources/fjterrain.hpp"
#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_shelter.hpp"
#include "renderer/resources/water_surface.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr float PI = 3.14159265358979f;

// A grid of `samples` a side over `size` metres with no margin, and land
// where `is_land(x, z)` says, in samples.
template <typename Fn>
water::ShelterLand grid(std::uint32_t samples, float size, Fn&& is_land) {
    water::ShelterLand land;
    land.samples = samples;
    land.size = size;
    land.land.assign(static_cast<std::size_t>(samples) * samples, 0);
    for (std::uint32_t z = 0; z < samples; ++z) {
        for (std::uint32_t x = 0; x < samples; ++x) {
            land.land[(static_cast<std::size_t>(z) * samples) + x] = is_land(x, z) ? 1 : 0;
        }
    }
    return land;
}

water::ShelterSources trains(std::initializer_list<water::ShelterSource> sources) {
    water::ShelterSources out;
    out.trains = sources;
    return out;
}

// The Gerstner trains' amplitude share at a sample, as the bake stored it.
float trains_share(const water::ShoreExposure& e, std::uint32_t x, std::uint32_t z) {
    return static_cast<float>(e.shares[2 * ((static_cast<std::size_t>(z) * e.samples) + x)])
         / 65535.0f;
}

} // namespace

TEST_CASE("Open sea is open everywhere", "[water_shelter]") {
    const auto land = grid(64, 126.0f, [](std::uint32_t, std::uint32_t) { return false; });
    water::ShelterSources sources;
    sources.trains = {{{1.0f, 0.0f}, 1.0f, 30.0f}, {{0.0f, -1.0f}, 0.5f, 12.0f}};
    sources.simulated = {{{0.6f, 0.8f}, 1.0f, 40.0f}};
    const water::ShoreExposure e = water::bake_exposure(land, sources, 1.0f);
    REQUIRE(e.shares.size() == 2u * 64 * 64);
    for (const std::uint16_t share : e.shares) CHECK(share == 65535);
}

TEST_CASE("Water behind a wall gets nothing of a sea it hides", "[water_shelter]") {
    // A wall across the whole grid at row 30, waves travelling +z into it,
    // short enough that their lee barely fills in from the grid's sides.
    const auto land = grid(101, 200.0f, [](std::uint32_t, std::uint32_t z) { return z == 30; });
    const water::ShoreExposure e =
        water::bake_exposure(land, trains({{{0.0f, 1.0f}, 1.0f, 1.0f}}), 1.0f);
    CHECK(trains_share(e, 50, 10) == 1.0f);   // in front of it
    CHECK(trains_share(e, 50, 30) == 0.0f);   // on it
    CHECK(trains_share(e, 50, 80) < 0.01f);   // behind it
}

TEST_CASE("A breakwater's lee widens as the square root of wavelength times distance",
          "[water_shelter]") {
    // Waves travelling +z past the end of a breakwater along row 20, from the
    // grid's edge to x = 100. Behind it the energy across the shadow's edge is
    // the diffusion of a step: half on the line through the end, and
    // 0.5 * erfc(-d / (sigma * sqrt 2)) at d to the side, sigma^2 = lambda r /
    // 2pi at r behind — worked out from that, not from the bake.
    constexpr float CELL = 2.0f;
    constexpr float WAVELENGTH = 40.0f;
    const auto land = grid(201, 400.0f, [](std::uint32_t x, std::uint32_t z) {
        return z == 20 && x <= 100;
    });
    const water::ShoreExposure e =
        water::bake_exposure(land, trains({{{0.0f, 1.0f}, 1.0f, WAVELENGTH}}), 1.0f);

    const std::uint32_t row = 120;   // 200 m behind
    const float behind = static_cast<float>(row - 20) * CELL;
    const float sigma = std::sqrt(WAVELENGTH * behind / (2.0f * PI));
    const float edge = 100.5f * CELL;   // between the last land sample and the first water
    for (const float d : {-sigma, -0.5f * sigma, 0.0f, 0.5f * sigma, sigma}) {
        const auto x = static_cast<std::uint32_t>(std::lround((edge + d) / CELL));
        const float at = (static_cast<float>(x) * CELL) - edge;
        const float expected = 0.5f * std::erfc(-at / (sigma * std::sqrt(2.0f)));
        const float share = trains_share(e, x, row);
        CHECK(share * share == Approx(expected).margin(0.04));
    }
}

TEST_CASE("Water hidden from one way the sea arrives takes the share of the others",
          "[water_shelter]") {
    // A wall across the grid hides the water behind it from waves travelling
    // +z, weighted 3, and not from waves travelling +x along it, weighted 1.
    // A quarter of the energy gets there: half the amplitude.
    const auto land = grid(101, 200.0f, [](std::uint32_t, std::uint32_t z) { return z == 30; });
    const water::ShoreExposure e = water::bake_exposure(
        land, trains({{{0.0f, 1.0f}, 3.0f, 1.0f}, {{1.0f, 0.0f}, 1.0f, 1.0f}}), 1.0f);
    CHECK(trains_share(e, 50, 80) == Approx(0.5).margin(0.01));
}

TEST_CASE("The exposure fades back to the open sea past the terrain", "[water_shelter]") {
    auto land = grid(101, 200.0f, [](std::uint32_t, std::uint32_t z) { return z == 30; });
    land.margin = 40.0f;
    const water::ShoreExposure e =
        water::bake_exposure(land, trains({{{0.0f, 1.0f}, 1.0f, 1.0f}}), 1.0f);
    CHECK(trains_share(e, 50, 100) == 1.0f);   // the grid's far edge: open sea past it
    CHECK(trains_share(e, 50, 60) < 0.01f);    // a margin's width in: the lee as baked
}

TEST_CASE("Land is where the ground stands above the water, however narrow", "[water_shelter]") {
    // A terrain 64 m across at half a metre, flat 5 m under the water, with a
    // ridge one sample wide down x = 32 m that stands 3 m out of it.
    auto data = std::make_shared<FjTerrainData>();
    data->header.sample_count = 129;
    data->header.world_size = 64.0f;
    data->header.height_scale = 10.0f;
    data->header.height_offset = -5.0f;
    data->heights.assign(129u * 129u, 0.0f);
    for (std::uint32_t z = 0; z < 129; ++z) data->heights[(z * 129u) + 64] = 0.8f;

    water::ShoreBed bed;
    bed.terrain = data;
    const water::ShelterLand land = water::shelter_land(bed);
    REQUIRE(land.samples >= 2);
    const float cell = land.size / static_cast<float>(land.samples - 1);
    CHECK(cell <= water::SHELTER_SPACING);

    // The ridge is a quarter of a cell wide and still land, all the way down.
    const auto ridge = static_cast<std::uint32_t>(std::lround((32.0f + land.margin) / cell));
    const std::uint32_t middle = land.samples / 2;
    CHECK(land.land[(static_cast<std::size_t>(middle) * land.samples) + ridge] == 1);
    CHECK(land.land[(static_cast<std::size_t>(middle) * land.samples) + ridge + 3] == 0);

    // Raise the water over the ridge's top and it is sea.
    bed.surface_y = 4.0f;
    const water::ShelterLand drowned = water::shelter_land(bed);
    CHECK(drowned.land[(static_cast<std::size_t>(middle) * drowned.samples) + ridge] == 0);
}

TEST_CASE("A simulated sea arrives across its spread, most of it downwind", "[water_shelter]") {
    FftContributor contributor;
    contributor.wind_direction = {1.0f, 0.0f};
    contributor.spread_degrees = 36.0f;
    contributor.resolution = 8;
    contributor.patch_size = 64.0f;
    FjWaterData water;
    water.simulated = water::draw_simulated_sea(contributor);

    const water::ShelterSources sources = water::shelter_sources(water);
    REQUIRE(sources.simulated.size() > 3);
    // The strongest source is the wind's own heading; the rest pair up either
    // side of it with equal weights, as the spreading is symmetric.
    const auto strongest = std::max_element(
        sources.simulated.begin(), sources.simulated.end(),
        [](const auto& a, const auto& b) { return a.weight < b.weight; });
    CHECK(strongest->travel.x == Approx(1.0));
    CHECK(strongest->travel.y == Approx(0.0).margin(1e-6));
    for (const auto& s : sources.simulated) {
        const auto mirror = std::find_if(sources.simulated.begin(), sources.simulated.end(),
                                         [&](const auto& m) {
                                             return std::abs(m.travel.x - s.travel.x) < 1e-5f
                                                 && std::abs(m.travel.y + s.travel.y) < 1e-5f;
                                         });
        REQUIRE(mirror != sources.simulated.end());
        CHECK(mirror->weight == s.weight);
    }
}

namespace {

// A bed with an island in the middle and a sea of one train to shelter from.
struct IslandSea {
    std::shared_ptr<FjTerrainData> terrain = std::make_shared<FjTerrainData>();
    water::ShoreBed bed;
    FjWaterData water;

    IslandSea() {
        terrain->header.sample_count = 65;
        terrain->header.world_size = 128.0f;
        terrain->header.height_scale = 10.0f;
        terrain->header.height_offset = -5.0f;
        terrain->heights.assign(65u * 65u, 0.0f);
        for (std::uint32_t z = 28; z < 36; ++z) {
            for (std::uint32_t x = 28; x < 36; ++x) terrain->heights[(z * 65u) + x] = 1.0f;
        }
        bed.terrain = terrain;

        GerstnerContributor sea;
        sea.waves = {WaterWave{{1.0f, 0.0f}, 20.0f, 0.5f, 0.5f, 1.0f}};
        FjWaterData authored;
        authored.contributors = {sea};
        water = water::derive_effective(authored, {0.0f, 0.0f});
    }
};

} // namespace

TEST_CASE("The shore derivation bakes once, and again when the land changes", "[water_shelter]") {
    IslandSea sea;
    water::ShoreDerivation derivation;

    const auto first = derivation.derive(sea.bed, sea.water, nullptr);
    REQUIRE(first != nullptr);
    CHECK(derivation.derive(sea.bed, sea.water, nullptr) == first);   // nothing changed

    // An edit to the terrain is a new generation, and a new bake.
    sea.bed.generation = 1;
    const auto edited = derivation.derive(sea.bed, sea.water, nullptr);
    CHECK(edited != first);

    // A flat sea has nothing to shelter.
    CHECK(derivation.derive(sea.bed, FjWaterData{}, nullptr) == nullptr);
}

TEST_CASE("The shore derivation bakes on a worker and hands out the last meanwhile",
          "[water_shelter]") {
    IslandSea sea;
    ThreadPool workers(2);

    // Baked without a worker: what a body of water copied into play had.
    water::ShoreDerivation earlier;
    const auto previous = earlier.derive(sea.bed, sea.water, nullptr);
    REQUIRE(previous != nullptr);

    water::ShoreDerivation derivation;
    CHECK(derivation.derive(sea.bed, sea.water, &workers, previous) == previous);

    std::shared_ptr<const water::ShoreExposure> baked;
    for (int i = 0; i < 500; ++i) {
        baked = derivation.derive(sea.bed, sea.water, &workers, previous);
        if (baked != previous) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(baked != previous);
    CHECK(baked->shares == previous->shares);   // the same land and sea bake the same
}

TEST_CASE("The lee kept is the share of it SHELTER_STRENGTH says", "[water_shelter]") {
    // Behind a wall the land takes all of a sea it hides; at half strength
    // half of what it took is given back, and the open water is untouched.
    const auto land = grid(101, 200.0f, [](std::uint32_t, std::uint32_t z) { return z == 30; });
    water::ShelterSources sources;
    sources.trains = {{{0.0f, 1.0f}, 1.0f, 1.0f}};
    const water::ShoreExposure half = water::bake_exposure(land, sources, 0.5f);
    CHECK(trains_share(half, 50, 10) == 1.0f);
    CHECK(trains_share(half, 50, 80) == Approx(0.5).margin(0.01));
    const water::ShoreExposure none = water::bake_exposure(land, sources, 0.0f);
    CHECK(trains_share(none, 50, 80) == 1.0f);
}
