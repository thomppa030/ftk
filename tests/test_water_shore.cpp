#include "renderer/resources/fjterrain.hpp"
#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_shore.hpp"
#include "renderer/resources/water_surface.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/resources/water_gpu.hpp"

#include <cmath>
#include <cstring>
#include <memory>

// The shader half of the shore, compiled as C++ so the two can be compared.
// See glsl_shim.hpp.
#include "glsl_shim.hpp"

// In a namespace of this file's own: other tests compile the wave field's
// shader too, and the copies must not meet at link time.
namespace shore_shim {

struct FjellWaterField;
FjellWaterField& shim_water_field();
float shim_bed(ivec2 texel);
vec2 shim_exposure(ivec2 texel);
#define FJELL_WATER_FIELD shim_water_field()
#define FJELL_WATER_BED(texel) shim_bed(texel)
#define FJELL_WATER_EXPOSURE(texel) shim_exposure(texel)
#include "../shaders/include/fjell_water_waves.glsl"
#include "../shaders/include/fjell_water_shore.glsl"

FjellWaterField& shim_water_field() {
    static FjellWaterField field{};
    return field;
}

// The heights the shader's bed texture holds, one per texel.
std::vector<float>& shim_bed_heights() {
    static std::vector<float> heights;
    return heights;
}

float shim_bed(ivec2 texel) {
    const auto samples = static_cast<int>(shim_water_field().shore.bed_samples);
    return shim_bed_heights()[static_cast<std::size_t>((texel.y * samples) + texel.x)];
}

// The exposure's shares, as its texture holds them.
std::vector<std::uint16_t>& shim_exposure_shares() {
    static std::vector<std::uint16_t> shares;
    return shares;
}

vec2 shim_exposure(ivec2 texel) {
    const auto samples = static_cast<int>(shim_water_field().shore.exposure_samples);
    const auto i = static_cast<std::size_t>(2 * ((texel.y * samples) + texel.x));
    return {static_cast<float>(shim_exposure_shares()[i]) / 65535.0f,
            static_cast<float>(shim_exposure_shares()[i + 1]) / 65535.0f};
}

} // namespace shore_shim

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
        const auto effect = water::shore_effect(water::StillDepth{row.depth, {0.0f, 0.0f}}, {}, {}, drawn.shoaling);
        REQUIRE(effect.has_value());
        CHECK(effect->simulated * drawn.shoaling.amplitude == Approx(row.amplitude).margin(0.01));
    }
}

TEST_CASE("The shore leaves deep water and a flat sea alone", "[water_shore]") {
    const water::ShoalingSum swell{2.0f * 3.14159265f / 100.0f, 1.0f};   // a 100 m wave
    const water::ShoalingSum chop{2.0f * 3.14159265f / 10.0f, 0.3f};     // a 10 m one

    // Deep for the longest wave present is deep for the sea.
    CHECK_FALSE(water::shore_effect(water::StillDepth{120.0f, {}}, {}, swell, chop).has_value());
    CHECK_FALSE(water::shore_effect(water::StillDepth{12.0f, {}}, {}, {}, chop).has_value());
    // A sea with nothing in it has nothing to shoal, however shallow.
    CHECK_FALSE(water::shore_effect(water::StillDepth{0.5f, {}}, {}, {}, {}).has_value());

    // Shallow for the swell, still deep for the chop: the chop does not grow
    // by any coefficient of its own, only shares what breaking keeps, which
    // at this depth is all of it.
    const auto mixed = water::shore_effect(water::StillDepth{12.0f, {}}, {}, swell, chop);
    REQUIRE(mixed.has_value());
    CHECK(mixed->simulated == 1.0f);
    CHECK(mixed->gerstner != 1.0f);
    CHECK(mixed->breaking == 0.0f);
}

TEST_CASE("There is no sea on dry ground", "[water_shore]") {
    const water::ShoalingSum sea{0.2f, 1.0f};
    for (const float depth : {0.0f, -0.1f, -30.0f}) {
        const auto effect = water::shore_effect(water::StillDepth{depth, {0.3f, 0.0f}}, {}, sea, sea);
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
        const auto at = water::shore_effect(water::StillDepth{depth, slope}, {}, swell, sea);
        REQUIRE(at.has_value());
        const float h = depth * 1e-3f;
        const auto deeper = water::shore_effect(water::StillDepth{depth + h, slope}, {}, swell, sea);
        const auto shallower = water::shore_effect(water::StillDepth{depth - h, slope}, {}, swell, sea);
        const float g_rate = (deeper->gerstner - shallower->gerstner) / (2.0f * h);
        const float s_rate = (deeper->simulated - shallower->simulated) / (2.0f * h);
        CHECK(at->gerstner_gradient.x == Approx(g_rate * slope.x).margin(1e-4));
        CHECK(at->gerstner_gradient.y == Approx(g_rate * slope.y).margin(1e-4));
        CHECK(at->simulated_gradient.x == Approx(s_rate * slope.x).margin(1e-4));
        CHECK(at->simulated_gradient.y == Approx(s_rate * slope.y).margin(1e-4));
    }
}

namespace {

// A lee over the beach and past it: the trains' share falling away toward +x
// and +z, the simulated sea's toward +x and rising toward +z. Planes, which
// bilinear blending reproduces exactly, so a finite difference across one
// sees no kink; sampled 10 m apart over 600 m from (-150, -300).
water::ShoreExposure lee() {
    water::ShoreExposure e;
    e.samples = 61;
    e.min_corner = {-150.0f, -300.0f};
    e.size = 600.0f;
    e.shares.resize(2u * 61u * 61u);
    for (std::uint32_t z = 0; z < 61; ++z) {
        for (std::uint32_t x = 0; x < 61; ++x) {
            const float wx = 10.0f * static_cast<float>(x);   // metres from the corner
            const float wz = 10.0f * static_cast<float>(z);
            const std::size_t i = 2 * ((static_cast<std::size_t>(z) * 61) + x);
            e.shares[i] = static_cast<std::uint16_t>(
                std::lround((0.95f - (0.001f * wx) - (0.0005f * wz)) * 65535.0f));
            e.shares[i + 1] = static_cast<std::uint16_t>(
                std::lround((0.8f - (0.0008f * wx) + (0.0002f * wz)) * 65535.0f));
        }
    }
    return e;
}

// A beach: a bed rising 1 in 30 toward +x, 10 m under the water at x = 0 and
// breaking the surface at x = 300. A plane, so the bed has one slope
// everywhere and the surface over it has no kinks for a finite difference to
// straddle.
water::ShoreBed beach() {
    auto data = terrain(129, 512.0f, 20.0f, -20.0f, [](std::uint32_t i, std::uint32_t) {
        // x = 4 i metres from the corner at x = -100; bed height -10 + x / 30.
        const float x = -100.0f + (4.0f * static_cast<float>(i));
        return ((-10.0f + (x / 30.0f)) + 20.0f) / 20.0f;
    });
    water::ShoreBed bed;
    bed.terrain = data;
    bed.min_corner = {-100.0f, -256.0f};
    bed.exposure = std::make_shared<const water::ShoreExposure>(lee());
    return bed;
}

// A sea of both kinds: three Gerstner trains and a simulated sea whose
// strongest waves physics sums, over the beach.
FjWaterData sea_on_the_beach() {
    GerstnerContributor trains;
    trains.waves = {
        WaterWave{{1.0f, 0.1f}, 48.0f, 0.9f, 0.6f, 1.0f},
        WaterWave{{0.8f, 0.5f}, 17.0f, 0.3f, 0.7f, 1.1f},
        WaterWave{{0.9f, -0.4f}, 9.0f, 0.12f, 0.5f, 0.9f},
    };
    FftContributor simulated;
    simulated.resolution = 32;
    simulated.patch_size = 96.0f;
    simulated.physics_waves = 24;
    simulated.amplitude_scale = 1.5f;

    FjWaterData authored;
    authored.contributors = {trains, simulated};
    FjWaterData water = water::derive_effective(authored, {0.0f, 0.0f});
    water.shore = beach();
    return water;
}

// Depths along the beach worth looking at: shoaling alone, the break point,
// deep in the breaking, and nearly on the sand.
constexpr float BEACH_X[] = {-60.0f, 120.0f, 180.0f, 240.0f, 270.0f, 290.0f};

glm::vec3 surface_point(const FjWaterData& water, glm::vec2 rest, float time) {
    const glm::vec3 d = water::displace(water, rest, time);
    return {rest.x + d.x, d.y, rest.y + d.z};
}

} // namespace

TEST_CASE("Near the shore the normal is the displaced surface's", "[water_shore]") {
    const FjWaterData water = sea_on_the_beach();
    for (const float x : BEACH_X) {
        for (const float time : {0.0f, 2.3f, 7.9f}) {
            const glm::vec2 rest{x, 13.0f};
            REQUIRE(water::shore_at(water, rest).has_value());

            // The surface's own tangents, from where it puts points either
            // side of this one.
            const float h = 0.01f;
            const glm::vec3 along_x = surface_point(water, rest + glm::vec2(h, 0.0f), time)
                                    - surface_point(water, rest - glm::vec2(h, 0.0f), time);
            const glm::vec3 along_z = surface_point(water, rest + glm::vec2(0.0f, h), time)
                                    - surface_point(water, rest - glm::vec2(0.0f, h), time);
            const glm::vec3 expected = glm::normalize(glm::cross(along_z, along_x));

            const glm::vec3 normal = water::normal_at(water, rest, time);
            CHECK(normal.x == Approx(expected.x).margin(2e-3));
            CHECK(normal.y == Approx(expected.y).margin(2e-3));
            CHECK(normal.z == Approx(expected.z).margin(2e-3));
        }
    }
}

TEST_CASE("Near the shore the fold measure is the displaced surface's", "[water_shore]") {
    const FjWaterData water = sea_on_the_beach();
    for (const float x : BEACH_X) {
        for (const float time : {0.0f, 2.3f, 7.9f}) {
            const glm::vec2 rest{x, -41.0f};
            const float h = 0.01f;
            const glm::vec3 along_x = surface_point(water, rest + glm::vec2(h, 0.0f), time)
                                    - surface_point(water, rest - glm::vec2(h, 0.0f), time);
            const glm::vec3 along_z = surface_point(water, rest + glm::vec2(0.0f, h), time)
                                    - surface_point(water, rest - glm::vec2(0.0f, h), time);
            // d(displaced x, z)/d(rest x, z), and its determinant.
            const float xx = along_x.x / (2.0f * h);
            const float zx = along_x.z / (2.0f * h);
            const float xz = along_z.x / (2.0f * h);
            const float zz = along_z.z / (2.0f * h);
            CHECK(water::horizontal_jacobian(water, rest, time)
                  == Approx((xx * zz) - (xz * zx)).margin(2e-3));
        }
    }
}

TEST_CASE("Near the shore the water's velocity is how its surface moves", "[water_shore]") {
    const FjWaterData water = sea_on_the_beach();
    for (const float x : BEACH_X) {
        const glm::vec2 rest{x, 5.0f};
        const float time = 4.2f;
        const float h = 1e-3f;
        const glm::vec3 expected = (water::displace(water, rest, time + h)
                                    - water::displace(water, rest, time - h))
                                 / (2.0f * h);
        const glm::vec3 velocity = water::surface_motion(water, rest, time).velocity;
        CHECK(velocity.x == Approx(expected.x).margin(5e-3));
        CHECK(velocity.y == Approx(expected.y).margin(5e-3));
        CHECK(velocity.z == Approx(expected.z).margin(5e-3));
    }
}

TEST_CASE("Near the shore the column's rest position is still found", "[water_shore]") {
    const FjWaterData water = sea_on_the_beach();
    for (const float x : BEACH_X) {
        for (const float time : {1.0f, 6.5f}) {
            const glm::vec2 column{x, 22.0f};
            const glm::vec2 rest = water::rest_position_for(water, column, time);
            const glm::vec3 d = water::displace(water, rest, time);
            CHECK(glm::length(rest + glm::vec2(d.x, d.z) - column) < 2e-3f);
        }
    }
}

TEST_CASE("The sea dies at the waterline and is gone on the sand", "[water_shore]") {
    const FjWaterData water = sea_on_the_beach();
    for (const float time : {0.0f, 3.3f, 9.1f}) {
        // On the sand the surface lies flat, under the ground.
        for (const float x : {300.0f, 305.0f, 380.0f}) {
            const water::SurfaceMotion m = water::surface_motion(water, {x, 0.0f}, time);
            CHECK(m.displacement == glm::vec3(0.0f));
            CHECK(m.velocity == glm::vec3(0.0f));
        }
        // And in the last few centimetres of water it is barely moving:
        // breaking holds the sea to 0.39 of the depth.
        const glm::vec3 d = water::displace(water, {299.4f, 0.0f}, time);
        CHECK(std::abs(d.y) < 0.39f * 0.02f * 3.0f);
    }
}

namespace {

// Put a field where the shader reads it, as the water pass would upload it,
// and its ground's heights where it reads those.
void give_the_shader(const FjWaterData& water) {
    const GpuWaterField packed = to_gpu_field(water);
    static_assert(sizeof(packed) == sizeof(shore_shim::FjellWaterField));
    std::memcpy(&shore_shim::shim_water_field(), &packed, sizeof(packed));
    shore_shim::shim_bed_heights() = water.shore->terrain->heights;
    shore_shim::shim_exposure_shares() =
        water.shore->exposure != nullptr ? water.shore->exposure->shares
                                         : std::vector<std::uint16_t>{};
}

// The sea on the beach with its Gerstner trains alone: the simulated sea
// reaches the shader as textures, which this cannot compile.
FjWaterData trains_on_the_beach() {
    FjWaterData water = sea_on_the_beach();
    water.simulated.reset();
    return water;
}

} // namespace

TEST_CASE("Shader and CPU agree on what the shore does", "[water_shore]") {
    // The whole sea, both sums: the factors do not need the textures.
    const FjWaterData water = sea_on_the_beach();
    give_the_shader(water);
    for (float x = -120.0f; x < 420.0f; x += 7.3f) {
        for (const float z : {-200.0f, 0.5f, 170.0f}) {
            const auto cpu = water::shore_at(water, {x, z});
            const shore_shim::FjellShoreEffect gpu = shore_shim::fjell_water_shore({x, z});
            if (!cpu.has_value()) {
                CHECK(gpu.gerstner == 1.0f);
                CHECK(gpu.simulated == 1.0f);
                CHECK(gpu.breaking == 0.0f);
                continue;
            }
            CHECK(gpu.gerstner == Approx(cpu->gerstner).margin(1e-5));
            CHECK(gpu.simulated == Approx(cpu->simulated).margin(1e-5));
            CHECK(gpu.gerstner_gradient.x == Approx(cpu->gerstner_gradient.x).margin(1e-5));
            CHECK(gpu.gerstner_gradient.y == Approx(cpu->gerstner_gradient.y).margin(1e-5));
            CHECK(gpu.simulated_gradient.x == Approx(cpu->simulated_gradient.x).margin(1e-5));
            CHECK(gpu.simulated_gradient.y == Approx(cpu->simulated_gradient.y).margin(1e-5));
            CHECK(gpu.breaking == Approx(cpu->breaking).margin(1e-5));
        }
    }
}

TEST_CASE("Shader and CPU agree on the sea over the shore", "[water_shore]") {
    // What the mesh stage makes of the trains: scaled, and their tangents and
    // fold terms carried through the shore, against the CPU's evaluators.
    const FjWaterData water = trains_on_the_beach();
    give_the_shader(water);
    for (float x = -60.0f; x < 310.0f; x += 11.9f) {
        for (const float time : {0.0f, 3.1f, 8.4f}) {
            const glm::vec2 rest{x, 9.0f};
            const shore_shim::FjellShoreEffect shore = shore_shim::fjell_water_shore(rest);
            const vec3 trains = shore_shim::fjell_water_displace(rest, time);

            const glm::vec3 displaced = water::displace(water, rest, time);
            const vec3 shaded = trains * shore.gerstner;
            CHECK(shaded.x == Approx(displaced.x).margin(1e-4));
            CHECK(shaded.y == Approx(displaced.y).margin(1e-4));
            CHECK(shaded.z == Approx(displaced.z).margin(1e-4));

            const vec3 normal = shore_shim::fjell_water_normal_from(
                shore_shim::fjell_water_shore_tangents(shore_shim::fjell_water_tangents(rest, time),
                                                       trains, shore.gerstner,
                                                       shore.gerstner_gradient));
            const glm::vec3 cpu_normal = water::normal_at(water, rest, time);
            CHECK(normal.x == Approx(cpu_normal.x).margin(1e-4));
            CHECK(normal.y == Approx(cpu_normal.y).margin(1e-4));
            CHECK(normal.z == Approx(cpu_normal.z).margin(1e-4));

            const float fold = shore_shim::fjell_water_determinant(
                shore_shim::fjell_water_shore_jacobian(
                    shore_shim::fjell_water_jacobian_terms(rest, time), trains, shore.gerstner,
                    shore.gerstner_gradient));
            CHECK(fold == Approx(water::horizontal_jacobian(water, rest, time)).margin(1e-4));
        }
    }
}

TEST_CASE("The GPU shore matches the shader's struct", "[water_shore]") {
    using shore_shim::FjellWaterField;
    using shore_shim::FjellWaterShore;
    REQUIRE(sizeof(GpuWaterShore) == sizeof(FjellWaterShore));
    REQUIRE(offsetof(GpuWaterField, shore) == offsetof(FjellWaterField, shore));
    REQUIRE(offsetof(GpuWaterShore, bed_min_corner) == offsetof(FjellWaterShore, bed_min_corner));
    REQUIRE(offsetof(GpuWaterShore, bed_size) == offsetof(FjellWaterShore, bed_size));
    REQUIRE(offsetof(GpuWaterShore, bed_samples) == offsetof(FjellWaterShore, bed_samples));
    REQUIRE(offsetof(GpuWaterShore, bed_height_scale)
            == offsetof(FjellWaterShore, bed_height_scale));
    REQUIRE(offsetof(GpuWaterShore, bed_height_offset)
            == offsetof(FjellWaterShore, bed_height_offset));
    REQUIRE(offsetof(GpuWaterShore, ground_y) == offsetof(FjellWaterShore, ground_y));
    REQUIRE(offsetof(GpuWaterShore, surface_y) == offsetof(FjellWaterShore, surface_y));
    REQUIRE(offsetof(GpuWaterShore, trains) == offsetof(FjellWaterShore, trains));
    REQUIRE(offsetof(GpuWaterShore, simulated) == offsetof(FjellWaterShore, simulated));
    REQUIRE(offsetof(GpuWaterShore, present) == offsetof(FjellWaterShore, present));
    REQUIRE(offsetof(GpuWaterShore, exposure_min_corner)
            == offsetof(FjellWaterShore, exposure_min_corner));
    REQUIRE(offsetof(GpuWaterShore, exposure_size) == offsetof(FjellWaterShore, exposure_size));
    REQUIRE(offsetof(GpuWaterShore, exposure_samples)
            == offsetof(FjellWaterShore, exposure_samples));
}

TEST_CASE("The exposure between its samples is their blend", "[water_shore]") {
    // lee() is two planes, so the blend anywhere on its grid is the planes'
    // value there, and its slope theirs.
    const water::ShoreExposure e = lee();
    for (const glm::vec2 at : {glm::vec2{-150.0f, -300.0f}, glm::vec2{3.7f, 41.2f},
                               glm::vec2{287.5f, -133.3f}, glm::vec2{450.0f, 300.0f}}) {
        const glm::vec2 from = at - e.min_corner;
        const water::ExposureAt open = water::exposure_at(e, at);
        CHECK(open.trains == Approx(0.95f - (0.001f * from.x) - (0.0005f * from.y)).margin(1e-4));
        CHECK(open.simulated == Approx(0.8f - (0.0008f * from.x) + (0.0002f * from.y)).margin(1e-4));
        CHECK(open.trains_gradient.x == Approx(-0.001).margin(1e-5));
        CHECK(open.trains_gradient.y == Approx(-0.0005).margin(1e-5));
        CHECK(open.simulated_gradient.x == Approx(-0.0008).margin(1e-5));
        CHECK(open.simulated_gradient.y == Approx(0.0002).margin(1e-5));
    }
    // Off its grid the sea is open.
    CHECK(water::exposure_at(e, {-151.0f, 0.0f}).open());
    CHECK(water::exposure_at(e, {0.0f, 301.0f}).open());
}

TEST_CASE("Deep water in a lee keeps the share that gets through", "[water_shore]") {
    const water::ShoalingSum swell{2.0f * 3.14159265f / 60.0f, 1.0f};
    const water::ShoalingSum sea{2.0f * 3.14159265f / 30.0f, 0.8f};
    water::ExposureAt open;
    open.trains = 0.4f;
    open.simulated = 0.7f;
    // Too deep to shoal, and with no depth at all: sheltered, not grown, not
    // broken.
    for (const auto& still : {std::optional<water::StillDepth>{water::StillDepth{200.0f, {}}},
                              std::optional<water::StillDepth>{}}) {
        const auto effect = water::shore_effect(still, open, swell, sea);
        REQUIRE(effect.has_value());
        CHECK(effect->gerstner == Approx(0.4f));
        CHECK(effect->simulated == Approx(0.7f));
        CHECK(effect->breaking == 0.0f);
    }
    // And wholly open deep water is left alone.
    CHECK_FALSE(water::shore_effect(std::nullopt, {}, swell, sea).has_value());
}

TEST_CASE("The shore factor's gradient follows the lee as well as the bed", "[water_shore]") {
    // Depth and both shares each a plane across the point; the factor taken
    // a little to either side along x and along z, from the planes, against
    // the gradient the shore states.
    const water::ShoalingSum swell{2.0f * 3.14159265f / 60.0f, 1.0f};
    const water::ShoalingSum sea{2.0f * 3.14159265f / 30.0f, 0.8f};
    const glm::vec2 depth_slope{0.02f, -0.05f};
    const glm::vec2 g_share_slope{-0.003f, 0.001f};
    const glm::vec2 s_share_slope{0.002f, -0.004f};

    for (const float depth : {20.0f, 5.0f, 2.5f, 0.8f}) {
        const auto at = [&](glm::vec2 offset) {
            water::ExposureAt open;
            open.trains = 0.6f + glm::dot(g_share_slope, offset);
            open.simulated = 0.5f + glm::dot(s_share_slope, offset);
            open.trains_gradient = g_share_slope;
            open.simulated_gradient = s_share_slope;
            return *water::shore_effect(
                water::StillDepth{depth + glm::dot(depth_slope, offset), depth_slope}, open,
                swell, sea);
        };
        const water::ShoreEffect here = at({0.0f, 0.0f});
        const float h = 0.01f;
        for (const glm::vec2 axis : {glm::vec2{1.0f, 0.0f}, glm::vec2{0.0f, 1.0f}}) {
            const water::ShoreEffect ahead = at(axis * h);
            const water::ShoreEffect behind = at(-axis * h);
            CHECK(glm::dot(here.gerstner_gradient, axis)
                  == Approx((ahead.gerstner - behind.gerstner) / (2.0f * h)).margin(2e-4));
            CHECK(glm::dot(here.simulated_gradient, axis)
                  == Approx((ahead.simulated - behind.simulated) / (2.0f * h)).margin(2e-4));
        }
    }
}
