#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/fjwater_io.hpp"
#include "renderer/resources/water_gpu.hpp"
#include "ftk/base/thread_pool.hpp"
#include "renderer/resources/water_derivation.hpp"
#include "renderer/resources/water_spectrum.hpp"
#include "renderer/resources/water_surface.hpp"

#include <glm/gtc/constants.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// The shader half of the wave field, compiled as C++ so the two evaluators
// can be compared directly. See glsl_shim.hpp for why this works and what it
// is limited to.
#include "glsl_shim.hpp"

// The shader's evaluators read the field where the including shader says it
// is, rather than taking it as an argument. Here that is a field the tests
// fill in; the overloads below put one there and evaluate it, so a test reads
// as "this field, at this point".
struct FjellWaterField;
FjellWaterField& shim_water_field();
#define FJELL_WATER_FIELD shim_water_field()
#include "../shaders/include/fjell_water_waves.glsl"

FjellWaterField& shim_water_field() {
    static FjellWaterField field{};
    return field;
}

static vec3 fjell_water_displace(const FjellWaterField& field, vec2 world_xz, float time) {
    shim_water_field() = field;
    return fjell_water_displace(world_xz, time);
}
static vec3 fjell_water_normal(const FjellWaterField& field, vec2 world_xz, float time) {
    shim_water_field() = field;
    return fjell_water_normal(world_xz, time);
}
static float fjell_water_jacobian(const FjellWaterField& field, vec2 world_xz, float time) {
    shim_water_field() = field;
    return fjell_water_jacobian(world_xz, time);
}
static float fjell_water_crest(const FjellWaterField& field, vec2 world_xz, float time) {
    shim_water_field() = field;
    return fjell_water_crest(world_xz, time);
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace fjell;
using Catch::Approx;

namespace {

/// A field of one Gerstner contributor carrying `waves`: what a `.fjwater`
/// holding nothing but a wave list is.
[[nodiscard]] FjWaterData sea(std::vector<WaterWave> waves = {}) {
    GerstnerContributor contributor;
    contributor.waves = std::move(waves);
    FjWaterData water;
    water.contributors.emplace_back(std::move(contributor));
    return water;
}

/// `water` as the world would run it: through apply_wind() with no wind, so
/// its trains are prepared for the point evaluators. Every field a test
/// evaluates goes through here, as every field the engine evaluates goes
/// through apply_wind().
[[nodiscard]] FjWaterData run(const FjWaterData& water) {
    return water::apply_wind(water, {0.0f, 0.0f});
}

[[nodiscard]] GerstnerContributor& gerstner(FjWaterData& water, std::size_t index = 0) {
    return std::get<GerstnerContributor>(water.contributors.at(index));
}

[[nodiscard]] const GerstnerContributor& gerstner(const FjWaterData& water,
                                                  std::size_t index = 0) {
    return std::get<GerstnerContributor>(water.contributors.at(index));
}

/// The same field, in the shape each evaluator wants.
struct FieldPair {
    FjWaterData cpu = sea();
    FjellWaterField gpu{};
};

[[nodiscard]] FieldPair make_field(const std::vector<WaterWave>& waves,
                                   float amplitude_scale = 1.0f,
                                   float speed_scale = 1.0f) {
    FieldPair pair;
    gerstner(pair.cpu).waves = waves;
    gerstner(pair.cpu).amplitude_scale = amplitude_scale;
    gerstner(pair.cpu).speed_scale = speed_scale;

    // Filled by hand rather than through to_gpu_field(), so that what the
    // shader does with a train that cannot move the surface is still put to
    // it: the packer would have dropped one before the shader ever saw it.
    auto& block = pair.gpu.gerstner[0];
    pair.gpu.gerstner_count = 1;
    block.wave_count = static_cast<unsigned int>(waves.size());
    block.amplitude_scale = amplitude_scale;
    block.speed_scale = speed_scale;
    block.steepness_headroom = gerstner(pair.cpu).steepness_headroom;
    for (std::size_t i = 0; i < waves.size() && i < MAX_WATER_WAVES; ++i) {
        block.waves[i].direction = waves[i].direction;
        block.waves[i].wavelength = waves[i].wavelength;
        block.waves[i].amplitude = waves[i].amplitude;
        block.waves[i].steepness = waves[i].steepness;
        block.waves[i].speed = waves[i].speed;
    }
    pair.cpu = run(pair.cpu);
    return pair;
}

[[nodiscard]] WaterWave wave(glm::vec2 dir, float wavelength, float amplitude,
                             float steepness, float speed = 1.0f) {
    WaterWave w;
    w.direction = dir;
    w.wavelength = wavelength;
    w.amplitude = amplitude;
    w.steepness = steepness;
    w.speed = speed;
    return w;
}

} // namespace

// --- The two evaluators agree ---
//
// This is the test the whole asset format exists for. Waves are authored in a
// .fjwater rather than written into GLSL so that physics can ask where the
// surface is; that only means anything while the shader and the CPU compute
// the same surface. Drift between them is a boat floating inside a wave, and
// it is invisible until someone looks at exactly the wrong moment.
//
// So evaluate both halves over a grid of positions and times and require they
// match. The GLSL is the real file, included and compiled, not a copy.

TEST_CASE("Shader and CPU wave fields agree", "[fjwater]") {
    auto field = make_field({
        wave({1.0f, 0.3f}, 28.0f, 1.4f, 0.85f, 1.0f),
        wave({-0.4f, 1.0f}, 13.0f, 0.6f, 0.7f, 1.3f),
        wave({0.7f, -0.8f}, 5.0f, 0.2f, 0.5f, 1.8f),
    }, 1.0f, 1.0f);

    for (float time : {0.0f, 0.37f, 2.5f, 11.0f, 120.0f}) {
        for (float x = -40.0f; x <= 40.0f; x += 7.0f) {
            for (float z = -40.0f; z <= 40.0f; z += 7.0f) {
                glm::vec2 p(x, z);
                INFO("at (" << x << ", " << z << ") t=" << time);

                // Margins are float noise, not slack. The two halves sum the
                // same terms through different compilers, and the error
                // accumulates over the wave list before anything normalizes.
                // Swept over this grid the worst disagreement measured
                // 1.4e-4 on displacement, 2.5e-3 on the normal (it divides by
                // a length, so it magnifies what came before) and 1.1e-4 on
                // the crest; these sit an order above that. A formula that
                // actually drifted would miss by far more than a wave's
                // width of a millimetre.
                glm::vec3 cpu_d = water::displace(run(field.cpu), p, time);
                vec3 gpu_d = fjell_water_displace(field.gpu, p, time);
                REQUIRE(cpu_d.x == Approx(gpu_d.x).margin(1e-3));
                REQUIRE(cpu_d.y == Approx(gpu_d.y).margin(1e-3));
                REQUIRE(cpu_d.z == Approx(gpu_d.z).margin(1e-3));

                glm::vec3 cpu_n = water::normal_at(run(field.cpu), p, time);
                vec3 gpu_n = fjell_water_normal(field.gpu, p, time);
                REQUIRE(cpu_n.x == Approx(gpu_n.x).margin(1e-2));
                REQUIRE(cpu_n.y == Approx(gpu_n.y).margin(1e-2));
                REQUIRE(cpu_n.z == Approx(gpu_n.z).margin(1e-2));

                REQUIRE(water::crest(run(field.cpu), p, time)
                        == Approx(fjell_water_crest(field.gpu, p, time)).margin(1e-3));
            }
        }
    }
}

TEST_CASE("A sea's dominant direction follows its biggest trains", "[fjwater]") {
    SECTION("one train travels the way it points") {
        auto field = make_field({wave({0.0f, 1.0f}, 20.0f, 1.0f, 0.5f)});
        glm::vec2 dir = water::dominant_direction(field.cpu);
        REQUIRE(dir.x == Approx(0.0f).margin(1e-5));
        REQUIRE(dir.y == Approx(1.0f).margin(1e-5));
    }

    SECTION("amplitude decides, not train count") {
        // Four ripples pulling one way do not outvote the swell carrying the
        // surface. This is the property that makes WAVE_DIR mean "the way
        // this sea runs" rather than "the average of its entries".
        auto field = make_field({
            wave({1.0f, 0.0f}, 45.0f, 2.0f, 0.5f),
            wave({0.0f, 1.0f}, 4.0f, 0.05f, 0.5f),
            wave({0.0f, 1.0f}, 4.0f, 0.05f, 0.5f),
            wave({0.0f, 1.0f}, 4.0f, 0.05f, 0.5f),
            wave({0.0f, 1.0f}, 4.0f, 0.05f, 0.5f),
        });
        glm::vec2 dir = water::dominant_direction(field.cpu);
        REQUIRE(dir.x > 0.99f);
        REQUIRE(dir.y == Approx(0.1f).margin(0.02f));
    }

    SECTION("opposed trains cancel to no direction") {
        // A sea running equally both ways has no direction of travel, and the
        // zero is the honest answer. Shaders fade their directional terms out
        // on it rather than dividing by it.
        auto field = make_field({
            wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.5f),
            wave({-1.0f, 0.0f}, 20.0f, 1.0f, 0.5f),
        });
        REQUIRE(glm::length(water::dominant_direction(field.cpu)) == Approx(0.0f).margin(1e-5));
    }

    SECTION("an empty field has no direction") {
        auto field = make_field({});
        REQUIRE(glm::length(water::dominant_direction(field.cpu)) == Approx(0.0f).margin(1e-5));
    }
}

TEST_CASE("The generated fan spans the spread it was given", "[fjwater]") {
    // The outermost train should reach spread_degrees. It used to stop short
    // of it — at six trains it reached three quarters — which left every
    // train near enough the wind that the crests read as parallel ridges and
    // foam ran along one axis however the shader composed it.
    WaterWaveGenerator gen;
    gen.wind_direction = {1.0f, 0.0f};
    gen.spread_degrees = 40.0f;

    for (std::uint32_t count : {2u, 3u, 4u, 5u, 6u, 7u, 8u, 16u}) {
        gen.wave_count = count;
        auto waves = generate_water_waves(gen);
        REQUIRE(waves.size() == count);

        float widest = 0.0f;
        for (const auto& w : waves) {
            widest = std::max(widest, std::abs(glm::degrees(std::atan2(w.direction.y,
                                                                      w.direction.x))));
        }
        INFO("wave_count=" << count << " widest=" << widest);
        REQUIRE(widest == Approx(40.0f).margin(0.01f));
    }

    SECTION("a single train travels straight downwind") {
        gen.wave_count = 1;
        auto waves = generate_water_waves(gen);
        REQUIRE(waves.size() == 1);
        REQUIRE(waves[0].direction.y == Approx(0.0f).margin(1e-5));
    }
}

TEST_CASE("Shader and CPU agree under the global scales", "[fjwater]") {
    // amplitude_scale and speed_scale are how a sea goes from calm to storm,
    // so they are applied in both halves and must be applied the same way.
    auto field = make_field({
        wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.8f),
        wave({0.3f, 0.9f}, 7.0f, 0.4f, 0.6f),
    }, 2.5f, 0.4f);

    for (float time : {0.0f, 3.3f, 40.0f}) {
        for (float x = -20.0f; x <= 20.0f; x += 5.0f) {
            glm::vec2 p(x, x * 0.5f);
            INFO("at x=" << x << " t=" << time);
            REQUIRE(water::displace(run(field.cpu), p, time).y
                    == Approx(fjell_water_displace(field.gpu, p, time).y).margin(1e-3));
            REQUIRE(water::crest(run(field.cpu), p, time)
                    == Approx(fjell_water_crest(field.gpu, p, time)).margin(1e-3));
        }
    }
}

TEST_CASE("Both halves drop a degenerate wave the same way", "[fjwater]") {
    // A zero direction or a zero-length wave has to be skipped identically or
    // the two surfaces differ by a whole train.
    auto field = make_field({
        wave({0.0f, 0.0f}, 20.0f, 1.0f, 0.5f),   // no direction
        wave({1.0f, 0.0f}, 0.0f, 1.0f, 0.5f),    // no wavelength
        wave({1.0f, 0.0f}, 15.0f, 0.8f, 0.5f),   // the only real one
    });

    for (float x = -10.0f; x <= 10.0f; x += 3.0f) {
        glm::vec2 p(x, 2.0f);
        REQUIRE(water::displace(run(field.cpu), p, 1.0f).y
                == Approx(fjell_water_displace(field.gpu, p, 1.0f).y).margin(1e-3));
    }
}

// --- A sea is the sum of its contributors ---

TEST_CASE("Splitting a sea across contributors leaves the surface where it was", "[fjwater]") {
    // Contributors are an authoring structure, not a different sea: the same
    // trains under the same scales sum to the same surface however they are
    // grouped. Exact rather than approximate, because the trains are summed in
    // the same order into the same accumulators either way.
    const std::vector<WaterWave> trains = {
        wave({1.0f, 0.3f}, 28.0f, 1.4f, 0.85f, 1.0f),
        wave({-0.4f, 1.0f}, 13.0f, 0.6f, 0.7f, 1.3f),
        wave({0.7f, -0.8f}, 5.0f, 0.2f, 0.5f, 1.8f),
    };
    FjWaterData whole = sea(trains);

    FjWaterData split = sea({trains[0]});
    split.contributors.emplace_back(GerstnerContributor{});
    gerstner(split, 1).waves = {trains[1], trains[2]};

    for (float time : {0.0f, 2.5f, 120.0f}) {
        for (float x = -40.0f; x <= 40.0f; x += 9.0f) {
            for (float z = -40.0f; z <= 40.0f; z += 9.0f) {
                glm::vec2 p(x, z);
                INFO("at (" << x << ", " << z << ") t=" << time);
                REQUIRE(water::displace(run(split), p, time) == water::displace(run(whole), p, time));
                REQUIRE(water::normal_at(run(split), p, time) == water::normal_at(run(whole), p, time));
                REQUIRE(water::horizontal_jacobian(run(split), p, time)
                        == water::horizontal_jacobian(run(whole), p, time));
            }
        }
    }
}

TEST_CASE("Each contributor's trains move under its own scales", "[fjwater]") {
    // Displacement is linear in the trains, so a sea of two contributors has
    // to land where the two seas taken alone land, added. It only does if
    // every train is evaluated under the scales, speed and headroom of the
    // contributor it belongs to and no other.
    FjWaterData swell = sea({wave({1.0f, 0.1f}, 60.0f, 1.5f, 0.6f)});
    gerstner(swell).amplitude_scale = 2.0f;
    gerstner(swell).speed_scale = 0.5f;
    gerstner(swell).steepness_headroom = 0.9f;

    FjWaterData chop = sea({
        wave({0.2f, 1.0f}, 6.0f, 0.2f, 0.9f, 1.4f),
        wave({-0.6f, 0.7f}, 3.5f, 0.1f, 0.8f, 1.7f),
    });
    gerstner(chop).amplitude_scale = 0.7f;
    gerstner(chop).speed_scale = 1.6f;
    gerstner(chop).steepness_headroom = 0.4f;

    FjWaterData both;
    both.contributors = {swell.contributors[0], chop.contributors[0]};

    for (float time : {0.0f, 3.3f, 41.0f}) {
        for (float x = -30.0f; x <= 30.0f; x += 7.0f) {
            glm::vec2 p(x, x * 0.6f - 4.0f);
            INFO("at x=" << x << " t=" << time);
            glm::vec3 together = water::displace(run(both), p, time);
            glm::vec3 apart = water::displace(run(swell), p, time) + water::displace(run(chop), p, time);
            REQUIRE(together.x == Approx(apart.x).margin(1e-4));
            REQUIRE(together.y == Approx(apart.y).margin(1e-4));
            REQUIRE(together.z == Approx(apart.z).margin(1e-4));
        }
    }

    // And the field-wide queries see both.
    REQUIRE(water::wave_height(both) == Approx(1.5f * 2.0f + (0.2f + 0.1f) * 0.7f));
    REQUIRE(water::shortest_wavelength(both) == Approx(3.5f));
    REQUIRE(water::bed_agitation(both, 3.0f)
            == Approx(water::bed_agitation(swell, 3.0f) + water::bed_agitation(chop, 3.0f)));
}

TEST_CASE("A contributor turned down to nothing stops steering the sea", "[fjwater]") {
    // The sea's direction is weighted by the amplitude a train actually has.
    // A swell muted through its contributor's scale no longer carries the
    // surface, so it must not still be what foam lines up on.
    FjWaterData water = sea({wave({1.0f, 0.0f}, 60.0f, 3.0f, 0.5f)});
    water.contributors.emplace_back(GerstnerContributor{});
    gerstner(water, 1).waves = {wave({0.0f, 1.0f}, 8.0f, 0.3f, 0.5f)};

    REQUIRE(water::dominant_direction(water).x > 0.99f);

    gerstner(water, 0).amplitude_scale = 0.0f;
    glm::vec2 dir = water::dominant_direction(water);
    REQUIRE(dir.x == Approx(0.0f).margin(1e-5));
    REQUIRE(dir.y == Approx(1.0f).margin(1e-5));
}

TEST_CASE("Contributors past the limit are not summed", "[fjwater]") {
    FjWaterData water;
    for (std::uint32_t i = 0; i < MAX_GERSTNER_CONTRIBUTORS; ++i) {
        GerstnerContributor contributor;
        contributor.waves = {wave({1.0f, 0.0f}, 20.0f + static_cast<float>(i), 0.5f, 0.5f)};
        water.contributors.emplace_back(std::move(contributor));
    }
    FjWaterData over = water;
    GerstnerContributor extra;
    extra.waves = {wave({0.0f, 1.0f}, 9.0f, 2.0f, 0.5f)};
    over.contributors.emplace_back(std::move(extra));

    glm::vec2 p(3.0f, -7.0f);
    REQUIRE(water::displace(run(over), p, 1.5f) == water::displace(run(water), p, 1.5f));
    REQUIRE(water::wave_height(over) == Approx(water::wave_height(water)));
    REQUIRE(water::shortest_wavelength(over) == Approx(20.0f));
}

// --- The field behaves like water ---

TEST_CASE("Flat water is flat", "[fjwater]") {
    FjWaterData water;  // no waves
    REQUIRE(water::height_at(run(water), {3.0f, 7.0f}, 5.0f) == Approx(0.0f));
    REQUIRE(water::normal_at(run(water), {3.0f, 7.0f}, 5.0f).y == Approx(1.0f));
    REQUIRE(water::crest(run(water), {3.0f, 7.0f}, 5.0f) == Approx(0.0f));
}

TEST_CASE("A wave stays within its amplitude", "[fjwater]") {
    FjWaterData water = sea();
    gerstner(water).waves = {wave({1.0f, 0.0f}, 20.0f, 1.5f, 0.9f)};

    float highest = -100.0f;
    float lowest = 100.0f;
    for (float x = 0.0f; x < 40.0f; x += 0.05f) {
        float h = water::height_at(run(water), {x, 0.0f}, 0.0f);
        highest = std::max(highest, h);
        lowest = std::min(lowest, h);
    }
    REQUIRE(highest == Approx(1.5f).margin(0.01));
    REQUIRE(lowest == Approx(-1.5f).margin(0.01));
}

TEST_CASE("Steeper waves pinch harder", "[fjwater]") {
    // The whole reason for Gerstner over sines: steepness has to actually
    // move the surface sideways, and the crest measure has to notice.
    auto peak_crest = [](float steepness) {
        FjWaterData water = sea();
        gerstner(water).waves = {wave({1.0f, 0.0f}, 20.0f, 1.0f, steepness)};
        float peak = 0.0f;
        for (float x = 0.0f; x < 20.0f; x += 0.05f) {
            peak = std::max(peak, water::crest(run(water), {x, 0.0f}, 0.0f));
        }
        return peak;
    };

    REQUIRE(peak_crest(0.0f) == Approx(0.0f).margin(1e-4));
    REQUIRE(peak_crest(0.5f) > peak_crest(0.1f));
    REQUIRE(peak_crest(1.0f) > peak_crest(0.5f));
}

TEST_CASE("Steepness cannot fold the surface over itself", "[fjwater]") {
    // Steepness is authored 0..1 precisely so a slider at its maximum still
    // gives a surface, rather than one that has turned inside out. The
    // Jacobian reaching exactly 0 is the limit; negative is folded.
    //
    // Asserted on the raw Jacobian rather than on crest(), which clamps to
    // 0..1 and so cannot report a fold at all: `crest(...) <= 1.0` holds for
    // every input including a surface inside out, and this test passed
    // vacuously for as long as it was written that way.
    FjWaterData water = sea();
    gerstner(water).waves = {wave({1.0f, 0.0f}, 10.0f, 2.0f, 1.0f)};

    float worst = 1.0f;
    for (float x = 0.0f; x < 20.0f; x += 0.01f) {
        worst = std::min(worst, water::horizontal_jacobian(run(water), {x, 0.0f}, 0.0f));
    }
    REQUIRE(worst >= 0.0f);
}

TEST_CASE("A field past its fold limit is reported as folded", "[fjwater]") {
    // The negative case, which is what makes the test above mean something.
    // Headroom is the dial that spends fold budget, so a field pushed far past
    // its ceiling must actually come back folded — if it does not, the fold
    // check is not measuring anything.
    FjWaterData water = sea();
    gerstner(water).waves = {wave({1.0f, 0.0f}, 10.0f, 2.0f, 1.0f)};
    gerstner(water).steepness_headroom = 4.0f;

    float worst = 1.0f;
    for (float x = 0.0f; x < 20.0f; x += 0.01f) {
        worst = std::min(worst, water::horizontal_jacobian(run(water), {x, 0.0f}, 0.0f));
    }
    REQUIRE(worst < 0.0f);
}

TEST_CASE("Both evaluators agree on the Jacobian", "[fjwater]") {
    // crest() is a clamp over this, so the pairing test on crest() cannot see
    // drift between the two once either side has folded. Pin the raw value.
    FieldPair field = make_field({
        wave({1.0f, 0.0f}, 12.0f, 1.0f, 0.9f),
        wave({0.3f, 0.8f}, 5.0f, 0.4f, 0.7f, 1.3f),
    });
    gerstner(field.cpu).steepness_headroom = 3.0f;
    field.gpu.gerstner[0].steepness_headroom = 3.0f;

    for (float time : {0.0f, 1.7f, 9.25f}) {
        for (float x = -20.0f; x <= 20.0f; x += 2.5f) {
            for (float z = -20.0f; z <= 20.0f; z += 2.5f) {
                glm::vec2 p{x, z};
                REQUIRE(water::horizontal_jacobian(run(field.cpu), p, time)
                        == Approx(fjell_water_jacobian(field.gpu, p, time)).margin(1e-3));
            }
        }
    }
}

TEST_CASE("The iterative height lands on the surface", "[fjwater]") {
    // height_at() reads the field at a rest position; under steep waves the
    // point actually above a world XZ started somewhere else. The iterative
    // form walks that back, so displacing from its answer should return to
    // the column it was asked about.
    FjWaterData water = sea();
    gerstner(water).waves = {
        wave({1.0f, 0.2f}, 18.0f, 1.2f, 0.9f),
        wave({-0.5f, 1.0f}, 9.0f, 0.5f, 0.8f),
    };

    // What has to hold is that the height returned belongs to a point of the
    // surface standing over the column asked about — that the solve landed on
    // the water rather than near it.
    //
    // Not that it agrees with some other solver. A steep Gerstner surface
    // bunches several rest positions over the same column and every one of
    // them is the real surface there; measured on this sea, a brute-force
    // sweep finds valid answers 0.7 m apart in height at two of these
    // samples. That is the surface overhanging, not a solver being wrong, and
    // nothing floating cares which branch it gets.
    for (float x = -15.0f; x <= 15.0f; x += 3.0f) {
        glm::vec2 target(x, x * 0.3f);
        INFO("at x=" << x);

        // Re-run the solve to recover the rest position behind the answer,
        // then confirm that position really does displace over the target and
        // really does carry that height.
        glm::vec2 rest = water::rest_position_for(run(water), target, 2.0f);
        glm::vec3 d = water::displace(run(water), rest, 2.0f);

        REQUIRE(rest.x + d.x == Approx(target.x).margin(1e-2));
        REQUIRE(rest.y + d.z == Approx(target.y).margin(1e-2));
        REQUIRE(d.y == Approx(water::height_at_iterative(run(water), target, 2.0f)).margin(1e-4));
    }
}

TEST_CASE("A sea travels in the direction its waves do", "[fjwater]") {
    // Sanity that time moves the surface the way the direction says: sample
    // ahead in time and the crest should have advanced downwind.
    FjWaterData water = sea();
    gerstner(water).waves = {wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.0f)};  // pure sine, no pinch

    auto crest_x = [&](float time) {
        float best_x = 0.0f;
        float best_h = -100.0f;
        for (float x = 0.0f; x < 20.0f; x += 0.01f) {
            float h = water::height_at(run(water), {x, 0.0f}, time);
            if (h > best_h) { best_h = h; best_x = x; }
        }
        return best_x;
    };

    REQUIRE(crest_x(0.1f) > crest_x(0.0f));
}

// --- What scripts are told about a sea ---

TEST_CASE("A sea's wave height is the sum of its trains", "[fjwater]") {
    // Scripts read this through WaterComponent::wave_height(), so what it means is part
    // of their ABI: a change here changes every game calling it, with no
    // compile error to say so. Pinned to numbers rather than to a formula
    // restated, so altering the meaning has to be done on purpose.
    FjWaterData water = sea();
    gerstner(water).waves = {
        wave({1.0f, 0.0f}, 40.0f, 1.25f, 0.5f),
        wave({0.0f, 1.0f}, 12.0f, 0.5f, 0.9f),
        wave({-1.0f, 1.0f}, 3.0f, 0.125f, 0.2f),
    };
    REQUIRE(water::wave_height(water) == Approx(1.875f));

    // The field's scale is part of how big the sea is; steepness, speed and
    // direction are not.
    gerstner(water).amplitude_scale = 2.0f;
    gerstner(water).speed_scale = 3.0f;
    gerstner(water).steepness_headroom = 0.9f;
    REQUIRE(water::wave_height(water) == Approx(3.75f));

    // After the wind, because a script asking how big the sea has got means
    // the sea the storm has made of it.
    gerstner(water).amplitude_scale = 1.0f;
    gerstner(water).wind.enabled = true;
    gerstner(water).wind.reference_speed = 8.0f;
    gerstner(water).wind.amplitude_response = 1.6f;
    REQUIRE(water::wave_height(water::apply_wind(water, {16.0f, 0.0f})) == Approx(3.0f));

    REQUIRE(water::wave_height(FjWaterData{}) == Approx(0.0f));
}

TEST_CASE("The shortest wavelength is the hardest train to draw", "[fjwater]") {
    FjWaterData water = sea();
    REQUIRE(water::shortest_wavelength(water) == Approx(0.0f));
    REQUIRE(water::shortest_wavelength(FjWaterData{}) == Approx(0.0f));

    gerstner(water).waves = {
        wave({1.0f, 0.0f}, 40.0f, 1.0f, 0.5f),
        wave({0.0f, 1.0f}, 3.5f, 0.1f, 0.5f),
        wave({1.0f, 1.0f}, 12.0f, 0.5f, 0.5f),
    };
    REQUIRE(water::shortest_wavelength(water) == Approx(3.5f));
}

// --- Generator ---

TEST_CASE("Wind leaves a sea alone until it is asked not to", "[fjwater]") {
    // Off by default, because most water is a pond, a canal or a pool and an
    // artist who has not thought about wind should get the sea they tuned.
    FjWaterData water = sea();
    gerstner(water).waves = {wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.5f)};
    REQUIRE_FALSE(gerstner(water).wind.enabled);

    FjWaterData blown = water::apply_wind(water, {30.0f, 0.0f});
    REQUIRE(gerstner(blown).amplitude_scale == Approx(gerstner(water).amplitude_scale));
    REQUIRE(gerstner(blown).steepness_headroom == Approx(gerstner(water).steepness_headroom));
    REQUIRE(gerstner(blown).waves[0].direction.x == Approx(gerstner(water).waves[0].direction.x));
    REQUIRE(gerstner(blown).waves[0].direction.y == Approx(gerstner(water).waves[0].direction.y));
}

TEST_CASE("The reference wind is the sea as authored", "[fjwater]") {
    // The authored field is the sea at the reference wind, not in a calm, so
    // an artist tuning at 8 m/s sees exactly what they tuned when the weather
    // blows 8 m/s. Anything else and the look they signed off on is a look
    // that never actually occurs.
    FjWaterData water = sea();
    gerstner(water).waves = {wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.5f)};
    gerstner(water).wind.enabled = true;
    gerstner(water).wind.reference_speed = 8.0f;

    FjWaterData same = water::apply_wind(water, {8.0f, 0.0f});
    REQUIRE(gerstner(same).amplitude_scale == Approx(gerstner(water).amplitude_scale));
    REQUIRE(gerstner(same).steepness_headroom == Approx(gerstner(water).steepness_headroom));
    REQUIRE(gerstner(same).waves[0].direction.x == Approx(1.0f));
    REQUIRE(gerstner(same).waves[0].direction.y == Approx(0.0f).margin(1e-6));
}

TEST_CASE("Wind raises and calms a sea", "[fjwater]") {
    FjWaterData water = sea();
    gerstner(water).waves = {wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.5f)};
    gerstner(water).wind.enabled = true;
    gerstner(water).wind.reference_speed = 8.0f;
    gerstner(water).wind.amplitude_response = 1.6f;
    gerstner(water).wind.direction_response = 0.0f;

    // Quoted at twice the reference, so double the wind is exactly the
    // response and the dial means what it says.
    REQUIRE(gerstner(water::apply_wind(water, {16.0f, 0.0f})).amplitude_scale == Approx(1.6f));
    // Half the reference is as far the other way.
    REQUIRE(gerstner(water::apply_wind(water, {4.0f, 0.0f})).amplitude_scale == Approx(0.7f));

    // A dead calm never inverts the sea, which a naive ramp through zero
    // would do, but it does not necessarily flatten it either: a swell
    // outlives the wind that raised it, and at the default response a
    // windless sea still stands at 40% of its authored height.
    REQUIRE(gerstner(water::apply_wind(water, {0.0f, 0.0f})).amplitude_scale == Approx(0.4f));

    // An artist who wants a millpond in a calm says so with a stronger
    // response, and the floor is what keeps that from going negative.
    gerstner(water).wind.amplitude_response = 4.0f;
    REQUIRE(gerstner(water::apply_wind(water, {0.0f, 0.0f})).amplitude_scale == Approx(0.0f));
}

TEST_CASE("Wind turns the chop and leaves the swell", "[fjwater]") {
    // The point of the direction response: a long swell was raised by a storm
    // somewhere else and holds its heading for days, while the chop on top
    // answers the wind it is actually in. One wind direction for the whole
    // sea would turn it like a sheet of plywood.
    FjWaterData water = sea();
    gerstner(water).waves = {
        wave({1.0f, 0.0f}, 60.0f, 1.0f, 0.5f),   // the swell
        wave({1.0f, 0.0f}, 30.0f, 0.5f, 0.5f),
        wave({1.0f, 0.0f}, 4.0f, 0.2f, 0.5f),    // the chop
    };
    gerstner(water).wind.enabled = true;
    gerstner(water).wind.direction_response = 1.0f;

    // Wind square across the authored heading, at the reference speed so only
    // direction is in play.
    FjWaterData blown = water::apply_wind(water, {0.0f, gerstner(water).wind.reference_speed});

    // The longest train does not move at all.
    REQUIRE(gerstner(blown).waves[0].direction.x == Approx(1.0f));
    REQUIRE(gerstner(blown).waves[0].direction.y == Approx(0.0f).margin(1e-6));
    // The shortest is fully onto the wind.
    REQUIRE(gerstner(blown).waves[2].direction.x == Approx(0.0f).margin(1e-6));
    REQUIRE(gerstner(blown).waves[2].direction.y == Approx(1.0f));
    // The middle one is partway, and still a unit vector — turning toward a
    // direction rather than lerping to it is what keeps this normalized.
    REQUIRE(gerstner(blown).waves[1].direction.y > 0.0f);
    REQUIRE(gerstner(blown).waves[1].direction.y < 1.0f);
    REQUIRE(glm::length(gerstner(blown).waves[1].direction) == Approx(1.0f));
}

TEST_CASE("Wind cannot leave a train without a direction", "[fjwater]") {
    // Turning a train onto a wind blowing straight back at it passes through
    // the midpoint where the two cancel. Normalizing a zero there would give
    // a NaN heading, and a NaN in the wave field is a sea that vanishes.
    FjWaterData water = sea();
    gerstner(water).waves = {
        wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.5f),
        wave({1.0f, 0.0f}, 10.0f, 0.5f, 0.5f),
        wave({1.0f, 0.0f}, 4.0f, 0.2f, 0.5f),
    };
    gerstner(water).wind.enabled = true;

    for (float response = 0.0f; response <= 1.0f; response += 0.01f) {
        gerstner(water).wind.direction_response = response;
        FjWaterData blown = water::apply_wind(water, {-gerstner(water).wind.reference_speed, 0.0f});
        for (const auto& w : gerstner(blown).waves) {
            INFO("direction_response " << response);
            REQUIRE(std::isfinite(w.direction.x));
            REQUIRE(std::isfinite(w.direction.y));
            REQUIRE(glm::length(w.direction) == Approx(1.0f));
        }
    }
}

TEST_CASE("A sea of one wavelength turns as one", "[fjwater]") {
    // The swell/chop ramp is relative to the wavelengths present, so a field
    // with nothing to distinguish has no swell to hold back and every train
    // answers the wind.
    FjWaterData water = sea();
    gerstner(water).waves = {
        wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.5f),
        wave({1.0f, 0.0f}, 20.0f, 0.5f, 0.5f),
    };
    gerstner(water).wind.enabled = true;
    gerstner(water).wind.direction_response = 1.0f;

    FjWaterData blown = water::apply_wind(water, {0.0f, gerstner(water).wind.reference_speed});
    for (const auto& w : gerstner(blown).waves) {
        REQUIRE(w.direction.y == Approx(1.0f));
    }
}

TEST_CASE("Each contributor answers the wind by its own response", "[fjwater]") {
    // What contributors are for: a swell raised by a storm somewhere else
    // rolls through unmoved while the chop beside it answers the wind it is
    // in. Each has to come out exactly as it would as a sea of its own —
    // neither borrowing the other's dials, nor measuring its swell-to-chop
    // ramp across the other's wavelengths.
    FjWaterData swell = sea({
        wave({1.0f, 0.0f}, 90.0f, 1.5f, 0.5f),
        wave({1.0f, 0.1f}, 60.0f, 0.8f, 0.5f),
    });

    FjWaterData chop = sea({
        wave({1.0f, 0.0f}, 12.0f, 0.3f, 0.7f),
        wave({1.0f, 0.0f}, 4.0f, 0.1f, 0.8f),
    });
    gerstner(chop).wind.enabled = true;
    gerstner(chop).wind.reference_speed = 6.0f;
    gerstner(chop).wind.amplitude_response = 2.0f;
    gerstner(chop).wind.direction_response = 1.0f;

    FjWaterData both;
    both.contributors = {swell.contributors[0], chop.contributors[0]};

    const glm::vec2 gale{0.0f, 12.0f};
    FjWaterData blown = water::apply_wind(both, gale);
    REQUIRE(blown.contributors.size() == 2);

    const GerstnerContributor& blown_swell = gerstner(blown, 0);
    REQUIRE(blown_swell.amplitude_scale == gerstner(swell).amplitude_scale);
    REQUIRE(blown_swell.waves[1].direction == gerstner(swell).waves[1].direction);

    const GerstnerContributor alone = gerstner(water::apply_wind(chop, gale));
    const GerstnerContributor& blown_chop = gerstner(blown, 1);
    REQUIRE(blown_chop.amplitude_scale == alone.amplitude_scale);
    REQUIRE(blown_chop.amplitude_scale == Approx(2.0f));
    REQUIRE(blown_chop.steepness_headroom == alone.steepness_headroom);
    for (std::size_t i = 0; i < alone.waves.size(); ++i) {
        REQUIRE(blown_chop.waves[i].direction == alone.waves[i].direction);
    }
    // Its longest train is the long end of its own ramp and holds its
    // heading, though the swell next door is seven times its length.
    REQUIRE(blown_chop.waves[0].direction.x == Approx(1.0f));
    REQUIRE(blown_chop.waves[1].direction.y == Approx(1.0f));
}

TEST_CASE("A sea takes time to answer the wind", "[fjwater]") {
    // Without the lag a weather event stepping its wind at a phase boundary
    // steps the whole sea with it, and the gust — which breathes about once a
    // second — heaves the entire ocean. The sea has to arrive late.
    const glm::vec2 calm{0.0f, 0.0f};
    const glm::vec2 gale{20.0f, 0.0f};

    glm::vec2 sea = calm;
    sea = water::advance_sea_wind(sea, gale, 1.0f);
    REQUIRE(sea.x > 0.0f);
    REQUIRE(sea.x < gale.x * 0.2f);   // nowhere near there after a second

    // And it does get there, given its time.
    for (int i = 0; i < 600; ++i) sea = water::advance_sea_wind(sea, gale, 0.5f);
    REQUIRE(sea.x == Approx(gale.x).margin(0.01f));
}

TEST_CASE("A sea dies slower than it builds", "[fjwater]") {
    // A wind raises a sea over hours; when it drops, the swell it made keeps
    // rolling for longer still. Asymmetric on purpose.
    const glm::vec2 gale{20.0f, 0.0f};

    glm::vec2 rising = glm::vec2(0.0f);
    glm::vec2 falling = gale;
    for (int i = 0; i < 20; ++i) {
        rising = water::advance_sea_wind(rising, gale, 1.0f);
        falling = water::advance_sea_wind(falling, glm::vec2(0.0f), 1.0f);
    }
    // After the same twenty seconds the rising sea has covered more of its
    // distance than the dying one has of its.
    const float risen = rising.x / gale.x;
    const float remaining = falling.x / gale.x;
    REQUIRE(risen > 1.0f - remaining);
}

TEST_CASE("The sea answers the same at any frame rate", "[fjwater]") {
    // An exponential approach written as a per-frame lerp is the classic way
    // to make a sea that builds faster on a better machine. Ten seconds is
    // ten seconds.
    const glm::vec2 gale{20.0f, 0.0f};

    glm::vec2 fast = glm::vec2(0.0f);
    for (int i = 0; i < 600; ++i) fast = water::advance_sea_wind(fast, gale, 1.0f / 60.0f);

    glm::vec2 slow = glm::vec2(0.0f);
    for (int i = 0; i < 100; ++i) slow = water::advance_sea_wind(slow, gale, 0.1f);

    glm::vec2 one_step = water::advance_sea_wind(glm::vec2(0.0f), gale, 10.0f);

    REQUIRE(fast.x == Approx(slow.x).margin(0.01f));
    REQUIRE(fast.x == Approx(one_step.x).margin(0.01f));
}

TEST_CASE("A sea reaches the bed only as deep as its wavelength", "[fjwater]") {
    // Orbital motion under a wave dies as exp(-k*d), so what clouds a coast
    // is the long trains: a chop and a swell of the same height do very
    // different things to a bed thirty metres down. Getting this backwards
    // would cloud the open ocean and leave the shore clear.
    FjWaterData chop = sea();
    gerstner(chop).waves = {wave({1.0f, 0.0f}, 5.0f, 1.0f, 0.5f)};

    FjWaterData swell = sea();
    gerstner(swell).waves = {wave({1.0f, 0.0f}, 150.0f, 1.0f, 0.5f)};

    // Shallow: both reach, and neither is idle.
    REQUIRE(water::bed_agitation(chop, 1.5f) > 0.0f);
    REQUIRE(water::bed_agitation(swell, 1.5f) > 0.0f);

    // Deep: the swell still works the bed, the chop has nothing left.
    REQUIRE(water::bed_agitation(swell, 30.0f) > water::bed_agitation(chop, 30.0f));
    REQUIRE(water::bed_agitation(chop, 30.0f) == Approx(0.0f).margin(1e-4f));

    // Flat water stirs nothing at any depth, and neither does dry land.
    FjWaterData flat;
    REQUIRE(water::bed_agitation(flat, 2.0f) == Approx(0.0f));
    REQUIRE(water::bed_agitation(swell, 0.0f) == Approx(0.0f));
}

TEST_CASE("A bigger sea stirs a bed harder", "[fjwater]") {
    FjWaterData calm = sea();
    gerstner(calm).waves = {wave({1.0f, 0.0f}, 40.0f, 0.2f, 0.5f)};
    FjWaterData storm = sea();
    gerstner(storm).waves = {wave({1.0f, 0.0f}, 40.0f, 1.5f, 0.5f)};

    REQUIRE(water::bed_agitation(storm, 4.0f) > water::bed_agitation(calm, 4.0f));

    // And the same field scaled by the wind response stirs more than at rest,
    // which is what makes a storm cloud the water and not merely raise it.
    FjWaterData blown = calm;
    gerstner(blown).amplitude_scale = 3.0f;
    REQUIRE(water::bed_agitation(blown, 4.0f) > water::bed_agitation(calm, 4.0f));
}

TEST_CASE("Silt settles far slower than it stirs", "[fjwater]") {
    // The whole point of tracking a load rather than reading the wind: water
    // does not go clear the moment a squall passes. Clouding is minutes,
    // clearing is hours.
    float stirring = 0.0f;
    float settling = 1.0f;
    for (int i = 0; i < 30; ++i) {
        stirring = water::advance_silt(stirring, 1.0f, 1.0f);
        settling = water::advance_silt(settling, 0.0f, 1.0f);
    }
    // After the same thirty seconds the clouding has covered much more of its
    // distance than the clearing has of its.
    REQUIRE(stirring > 1.0f - settling);

    // It does get there eventually, both ways.
    for (int i = 0; i < 2000; ++i) stirring = water::advance_silt(stirring, 1.0f, 1.0f);
    REQUIRE(stirring == Approx(1.0f).margin(0.01f));
    for (int i = 0; i < 5000; ++i) settling = water::advance_silt(settling, 0.0f, 1.0f);
    REQUIRE(settling == Approx(0.0f).margin(0.01f));
}

TEST_CASE("Silt answers the same at any frame rate", "[fjwater]") {
    // Same trap as the sea's own lag: a per-frame lerp would cloud the water
    // faster on a better machine.
    float fast = 0.0f;
    for (int i = 0; i < 600; ++i) fast = water::advance_silt(fast, 1.0f, 1.0f / 60.0f);

    float slow = 0.0f;
    for (int i = 0; i < 100; ++i) slow = water::advance_silt(slow, 1.0f, 0.1f);

    const float one_step = water::advance_silt(0.0f, 1.0f, 10.0f);

    REQUIRE(fast == Approx(slow).margin(0.005f));
    REQUIRE(fast == Approx(one_step).margin(0.005f));
}

TEST_CASE("Wind response survives a round trip", "[fjwater]") {
    FjWaterData water = sea();
    gerstner(water).waves = {wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.5f)};
    gerstner(water).wind.enabled = true;
    gerstner(water).wind.reference_speed = 11.0f;
    gerstner(water).wind.amplitude_response = 2.2f;
    gerstner(water).wind.steepness_response = 1.4f;
    gerstner(water).wind.direction_response = 0.35f;

    auto path = std::filesystem::temp_directory_path() / "fjell_wind_roundtrip.fjwater";
    REQUIRE(save_fjwater_file(path.string(), water));
    auto loaded = load_fjwater_file(path.string());
    REQUIRE(loaded);
    REQUIRE(gerstner(*loaded).wind.enabled);
    REQUIRE(gerstner(*loaded).wind.reference_speed == Approx(11.0f));
    REQUIRE(gerstner(*loaded).wind.amplitude_response == Approx(2.2f));
    REQUIRE(gerstner(*loaded).wind.steepness_response == Approx(1.4f));
    REQUIRE(gerstner(*loaded).wind.direction_response == Approx(0.35f));
    std::filesystem::remove(path);
}

TEST_CASE("The generator is deterministic", "[fjwater]") {
    WaterWaveGenerator settings;
    settings.wind_direction = {0.6f, 0.8f};
    settings.swell = 1.8f;
    settings.wave_count = 6;

    auto a = generate_water_waves(settings);
    auto b = generate_water_waves(settings);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].wavelength == Approx(b[i].wavelength));
        REQUIRE(a[i].amplitude == Approx(b[i].amplitude));
        REQUIRE(a[i].direction.x == Approx(b[i].direction.x));
    }
}

TEST_CASE("Generated waves descend in scale", "[fjwater]") {
    WaterWaveGenerator settings;
    settings.wave_count = 6;
    settings.longest_wavelength = 40.0f;
    settings.swell = 2.0f;

    auto waves = generate_water_waves(settings);
    REQUIRE(waves.size() == 6);
    REQUIRE(waves[0].wavelength == Approx(40.0f));
    for (std::size_t i = 1; i < waves.size(); ++i) {
        INFO("train " << i);
        REQUIRE(waves[i].wavelength < waves[i - 1].wavelength);
        REQUIRE(waves[i].amplitude < waves[i - 1].amplitude);
    }
}

TEST_CASE("The generator honours its bounds", "[fjwater]") {
    WaterWaveGenerator settings;
    settings.wave_count = 999;
    REQUIRE(generate_water_waves(settings).size() == MAX_WATER_WAVES);

    settings.wave_count = 0;
    REQUIRE(generate_water_waves(settings).empty());

    // A dead wind still has to produce a sea rather than a divide by zero.
    settings.wave_count = 4;
    settings.wind_direction = {0.0f, 0.0f};
    auto waves = generate_water_waves(settings);
    REQUIRE(waves.size() == 4);
    REQUIRE(glm::length(waves[0].direction) == Approx(1.0f).margin(1e-4));
}

TEST_CASE("Generated trains fan out around the wind", "[fjwater]") {
    WaterWaveGenerator settings;
    settings.wind_direction = {1.0f, 0.0f};
    settings.wave_count = 5;
    settings.spread_degrees = 45.0f;

    auto waves = generate_water_waves(settings);
    // The first train runs straight downwind and carries the swell.
    REQUIRE(waves[0].direction.x == Approx(1.0f).margin(1e-4));
    REQUIRE(waves[0].direction.y == Approx(0.0f).margin(1e-4));

    // No train strays further than the spread allows.
    for (const auto& w : waves) {
        float angle = std::acos(std::clamp(glm::dot(glm::normalize(w.direction),
                                                    glm::vec2(1.0f, 0.0f)), -1.0f, 1.0f));
        INFO("angle " << glm::degrees(angle));
        REQUIRE(glm::degrees(angle) <= 45.0f + 1e-3f);
    }
}

// --- File round trip ---

TEST_CASE("A .fjwater survives a round trip", "[fjwater]") {
    auto dir = std::filesystem::temp_directory_path() / "fjell-fjwater-test";
    std::filesystem::create_directories(dir);
    auto path = (dir / "sea.fjwater").string();

    FjWaterData written = sea();
    gerstner(written).waves = {
        wave({1.0f, 0.3f}, 28.0f, 1.4f, 0.85f, 1.1f),
        wave({-0.4f, 1.0f}, 13.0f, 0.6f, 0.7f, 1.3f),
    };
    gerstner(written).amplitude_scale = 1.7f;
    gerstner(written).speed_scale = 0.8f;
    gerstner(written).generator.wind_direction = {0.6f, 0.8f};
    gerstner(written).generator.swell = 2.2f;
    gerstner(written).generator.wave_count = 5;

    REQUIRE(save_fjwater_file(path, written));

    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(gerstner(*loaded).waves.size() == 2);
    REQUIRE(gerstner(*loaded).amplitude_scale == Approx(1.7f));
    REQUIRE(gerstner(*loaded).speed_scale == Approx(0.8f));
    REQUIRE(gerstner(*loaded).waves[0].wavelength == Approx(28.0f));
    REQUIRE(gerstner(*loaded).waves[0].steepness == Approx(0.85f));
    REQUIRE(gerstner(*loaded).waves[1].speed == Approx(1.3f));
    // The generator settings come back so the editor can generate again.
    REQUIRE(gerstner(*loaded).generator.swell == Approx(2.2f));
    REQUIRE(gerstner(*loaded).generator.wave_count == 5);

    std::filesystem::remove_all(dir);
}

TEST_CASE("An old .fjwater keeps the surface it was tuned to", "[fjwater]") {
    // Before the steepness budget stopped being divided by the train count,
    // a file's headroom was a multiplier on a 1/N share. Loading one of those
    // at face value would put every train N times steeper than the artist saw
    // and fold the sea on open. The migration divides it back, and what pins
    // that is the surface itself: the same field, evaluated either side of the
    // change, has to land in the same place.
    auto dir = std::filesystem::temp_directory_path() / "fjell-fjwater-legacy";
    std::filesystem::create_directories(dir);
    auto path = (dir / "old.fjwater").string();

    // A pre-version file: no "version" key, headroom meaning a 1/N share.
    std::ofstream(path) << R"({
        "amplitude_scale": 1.0,
        "speed_scale": 1.0,
        "steepness_headroom": 1.1,
        "waves": [
            {"direction": [1.0, 0.0], "wavelength": 30.0, "amplitude": 1.0,
             "steepness": 0.45, "speed": 1.0},
            {"direction": [0.5, 0.8], "wavelength": 12.0, "amplitude": 0.5,
             "steepness": 0.6, "speed": 1.1}
        ]
    })";

    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(gerstner(*loaded).steepness_headroom == Approx(1.1f / 2.0f));

    // It predates contributors too, and is one: everything it kept at its
    // root belongs to a single Gerstner contributor.
    REQUIRE(loaded->contributors.size() == 1);
    REQUIRE(gerstner(*loaded).waves.size() == 2);
    REQUIRE(gerstner(*loaded).waves[1].wavelength == Approx(12.0f));

    // A file the current writer produced is taken at face value.
    auto fresh_path = (dir / "new.fjwater").string();
    REQUIRE(save_fjwater_file(fresh_path, *loaded));
    auto reloaded = load_fjwater_file(fresh_path);
    REQUIRE(reloaded);
    REQUIRE(gerstner(*reloaded).steepness_headroom == Approx(gerstner(*loaded).steepness_headroom));

    // And rewriting it moved nothing: the surface read back from the current
    // format is exactly the one the old file gave.
    REQUIRE(reloaded->contributors.size() == 1);
    for (float x = -20.0f; x <= 20.0f; x += 4.0f) {
        glm::vec2 p(x, 3.0f - x * 0.5f);
        REQUIRE(water::displace(run(*reloaded), p, 6.5f) == water::displace(run(*loaded), p, 6.5f));
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("A .fjwater from before contributors reads as one", "[fjwater]") {
    // The format that kept a single sea at the root of the file, with its
    // version saying the headroom already means what it says. Every key at
    // the root is that one contributor's.
    auto dir = std::filesystem::temp_directory_path() / "fjell-fjwater-v2";
    std::filesystem::create_directories(dir);
    auto path = (dir / "v2.fjwater").string();

    std::ofstream(path) << R"({
        "version": 2,
        "amplitude_scale": 1.3,
        "speed_scale": 0.9,
        "steepness_headroom": 0.8,
        "preview_material": "assets/materials/sea.fjmat",
        "wind": {"enabled": true, "reference_speed": 11.0, "amplitude_response": 2.0,
                 "steepness_response": 1.5, "direction_response": 0.25},
        "generator": {"swell": 2.5, "wave_count": 3},
        "waves": [
            {"direction": [1.0, 0.2], "wavelength": 42.0, "amplitude": 1.1,
             "steepness": 0.8, "speed": 1.0},
            {"direction": [0.6, -0.7], "wavelength": 17.0, "amplitude": 0.5,
             "steepness": 0.7, "speed": 1.1}
        ]
    })";

    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(loaded->preview_material == "assets/materials/sea.fjmat");
    REQUIRE(loaded->contributors.size() == 1);

    const GerstnerContributor& only = gerstner(*loaded);
    REQUIRE(only.amplitude_scale == Approx(1.3f));
    REQUIRE(only.speed_scale == Approx(0.9f));
    REQUIRE(only.steepness_headroom == Approx(0.8f));   // versioned: taken at face value
    REQUIRE(only.wind.enabled);
    REQUIRE(only.wind.reference_speed == Approx(11.0f));
    REQUIRE(only.wind.direction_response == Approx(0.25f));
    REQUIRE(only.generator.swell == Approx(2.5f));
    REQUIRE(only.generator.wave_count == 3);
    REQUIRE(only.waves.size() == 2);
    REQUIRE(only.waves[1].wavelength == Approx(17.0f));

    std::filesystem::remove_all(dir);
}

TEST_CASE("Several contributors survive a round trip", "[fjwater]") {
    auto dir = std::filesystem::temp_directory_path() / "fjell-fjwater-contributors";
    std::filesystem::create_directories(dir);
    auto path = (dir / "two.fjwater").string();

    FjWaterData written = sea({wave({1.0f, 0.1f}, 60.0f, 1.5f, 0.6f)});
    gerstner(written).amplitude_scale = 2.0f;
    gerstner(written).generator.swell = 3.0f;
    written.contributors.emplace_back(GerstnerContributor{});
    gerstner(written, 1).waves = {
        wave({0.2f, 1.0f}, 6.0f, 0.2f, 0.9f, 1.4f),
        wave({-0.6f, 0.7f}, 3.5f, 0.1f, 0.8f, 1.7f),
    };
    gerstner(written, 1).speed_scale = 1.6f;
    gerstner(written, 1).steepness_headroom = 0.4f;
    gerstner(written, 1).wind.enabled = true;
    gerstner(written, 1).wind.direction_response = 0.9f;
    written.preview_material = "assets/materials/sea.fjmat";

    REQUIRE(save_fjwater_file(path, written));
    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(loaded->preview_material == written.preview_material);
    REQUIRE(loaded->contributors.size() == 2);

    // In the order they were written, each with what was its own.
    REQUIRE(gerstner(*loaded, 0).amplitude_scale == Approx(2.0f));
    REQUIRE(gerstner(*loaded, 0).generator.swell == Approx(3.0f));
    REQUIRE_FALSE(gerstner(*loaded, 0).wind.enabled);
    REQUIRE(gerstner(*loaded, 1).waves.size() == 2);
    REQUIRE(gerstner(*loaded, 1).speed_scale == Approx(1.6f));
    REQUIRE(gerstner(*loaded, 1).steepness_headroom == Approx(0.4f));
    REQUIRE(gerstner(*loaded, 1).wind.enabled);
    REQUIRE(gerstner(*loaded, 1).wind.direction_response == Approx(0.9f));

    for (float x = -20.0f; x <= 20.0f; x += 4.0f) {
        glm::vec2 p(x, 1.0f + x * 0.25f);
        REQUIRE(water::displace(run(*loaded), p, 4.0f) == water::displace(run(written), p, 4.0f));
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("A contributor of an unknown kind fails the load", "[fjwater]") {
    // Skipping it would load a sea with a piece missing, and the next save
    // would write the file back without it.
    auto dir = std::filesystem::temp_directory_path() / "fjell-fjwater-kind";
    std::filesystem::create_directories(dir);
    auto path = (dir / "future.fjwater").string();

    std::ofstream(path) << R"({
        "version": 3,
        "contributors": [
            {"kind": "gerstner", "waves": [{"direction": [1.0, 0.0], "wavelength": 12.0}]},
            {"kind": "tidal"}
        ]
    })";
    REQUIRE_FALSE(load_fjwater_file(path));

    std::filesystem::remove_all(dir);
}

TEST_CASE("A missing .fjwater is an error, not an empty sea", "[fjwater]") {
    REQUIRE_FALSE(load_fjwater_file("/nonexistent/nowhere.fjwater"));
}

TEST_CASE("A .fjwater drops waves it cannot use", "[fjwater]") {
    auto dir = std::filesystem::temp_directory_path() / "fjell-fjwater-bad";
    std::filesystem::create_directories(dir);
    auto path = (dir / "bad.fjwater").string();

    std::ofstream(path) << R"({
        "waves": [
            { "direction": [0.0, 0.0], "wavelength": 20.0 },
            { "direction": [1.0, 0.0], "wavelength": 0.0 },
            { "direction": [1.0, 0.0], "wavelength": 12.0, "amplitude": 0.5 }
        ]
    })";

    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(gerstner(*loaded).waves.size() == 1);
    REQUIRE(gerstner(*loaded).waves[0].wavelength == Approx(12.0f));

    std::filesystem::remove_all(dir);
}

TEST_CASE("A .fjwater keeps at most MAX_WATER_WAVES trains", "[fjwater]") {
    auto dir = std::filesystem::temp_directory_path() / "fjell-fjwater-many";
    std::filesystem::create_directories(dir);
    auto path = (dir / "many.fjwater").string();

    FjWaterData written = sea();
    for (int i = 0; i < 40; ++i) {
        gerstner(written).waves.push_back(wave({1.0f, 0.0f}, 10.0f + static_cast<float>(i), 0.1f, 0.5f));
    }
    REQUIRE(save_fjwater_file(path, written));

    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(gerstner(*loaded).waves.size() == MAX_WATER_WAVES);

    std::filesystem::remove_all(dir);
}

// --- The buffer the shader reads ---
//
// The field reaches the GPU as a uniform buffer, so a third description of it
// exists: the C++ struct that fills the buffer, next to the GLSL struct that
// reads it. Matching sizeof() is not enough to know two layouts agree — the
// engine has been caught by that before — so compare them member by member,
// which is possible here because the shim compiles the real GLSL as C++.

TEST_CASE("The GPU field matches the shader's struct", "[fjwater]") {
    REQUIRE(sizeof(GpuWaterWave) == sizeof(FjellWaterWave));
    REQUIRE(sizeof(GpuGerstnerContributor) == sizeof(FjellGerstnerContributor));
    REQUIRE(sizeof(GpuWaterField) == sizeof(FjellWaterField));

    REQUIRE(offsetof(GpuWaterWave, direction) == offsetof(FjellWaterWave, direction));
    REQUIRE(offsetof(GpuWaterWave, wavelength) == offsetof(FjellWaterWave, wavelength));
    REQUIRE(offsetof(GpuWaterWave, amplitude) == offsetof(FjellWaterWave, amplitude));
    REQUIRE(offsetof(GpuWaterWave, steepness) == offsetof(FjellWaterWave, steepness));
    REQUIRE(offsetof(GpuWaterWave, speed) == offsetof(FjellWaterWave, speed));

    REQUIRE(offsetof(GpuGerstnerContributor, waves) == offsetof(FjellGerstnerContributor, waves));
    REQUIRE(offsetof(GpuGerstnerContributor, wave_count)
            == offsetof(FjellGerstnerContributor, wave_count));
    REQUIRE(offsetof(GpuGerstnerContributor, amplitude_scale)
            == offsetof(FjellGerstnerContributor, amplitude_scale));
    REQUIRE(offsetof(GpuGerstnerContributor, speed_scale)
            == offsetof(FjellGerstnerContributor, speed_scale));
    REQUIRE(offsetof(GpuGerstnerContributor, steepness_headroom)
            == offsetof(FjellGerstnerContributor, steepness_headroom));

    REQUIRE(offsetof(GpuWaterField, gerstner) == offsetof(FjellWaterField, gerstner));
    REQUIRE(offsetof(GpuWaterField, gerstner_count) == offsetof(FjellWaterField, gerstner_count));
    REQUIRE(offsetof(GpuWaterField, simulated_patch_size)
            == offsetof(FjellWaterField, simulated_patch_size));

    // The array strides are what a matching sizeof() can still hide: two
    // structs of equal size whose arrays step differently put everything
    // after the first element in the wrong place.
    GpuWaterField cpu{};
    FjellWaterField gpu{};
    auto stride = [](const auto& first, const auto& second) {
        return reinterpret_cast<const char*>(&second) - reinterpret_cast<const char*>(&first);
    };
    REQUIRE(stride(cpu.gerstner[0], cpu.gerstner[1]) == stride(gpu.gerstner[0], gpu.gerstner[1]));
    REQUIRE(stride(cpu.gerstner[0].waves[0], cpu.gerstner[0].waves[1])
            == stride(gpu.gerstner[0].waves[0], gpu.gerstner[0].waves[1]));
}

namespace {

/// What the pass uploads, as the shader's own struct: the bytes the GPU would
/// read, handed to the shader's own code.
[[nodiscard]] FjellWaterField as_shader_reads(const FjWaterData& water) {
    static_assert(sizeof(GpuWaterField) == sizeof(FjellWaterField));
    GpuWaterField packed = to_gpu_field(water);
    FjellWaterField field{};
    std::memcpy(&field, &packed, sizeof(field));
    return field;
}

} // namespace

TEST_CASE("Packing for the GPU keeps the sea the CPU sees", "[fjwater]") {
    // The whole path, with nothing built by hand to agree with itself: the
    // field goes through the packer the pass uses, the bytes it would upload
    // are read as the shader's struct, and the shader's code evaluates them.
    // The CPU evaluator is given the authored field and has to land in the
    // same place — across contributors with their own scales, trains that
    // cannot move the surface, more trains than a contributor may sum and
    // more contributors than a field may.
    FjWaterData water = sea({
        wave({1.0f, 0.3f}, 28.0f, 1.4f, 0.85f, 1.1f),
        wave({0.0f, 0.0f}, 20.0f, 1.0f, 0.5f),    // no direction
        wave({-0.4f, 1.0f}, 13.0f, 0.6f, 0.7f, 1.3f),
        wave({1.0f, 0.0f}, 0.0f, 1.0f, 0.5f),     // no wavelength
    });
    gerstner(water).amplitude_scale = 1.6f;
    gerstner(water).speed_scale = 0.7f;

    // Dead trains ahead of live ones, and past the limit once they are gone.
    GerstnerContributor crowded;
    crowded.amplitude_scale = 0.5f;
    crowded.steepness_headroom = 0.9f;
    for (int i = 0; i < 6; ++i) crowded.waves.push_back(wave({0.0f, 0.0f}, 9.0f, 0.4f, 0.5f));
    for (int i = 0; i < 20; ++i) {
        float f = static_cast<float>(i);
        crowded.waves.push_back(wave({std::cos(f), std::sin(f)}, 3.0f + f, 0.05f + 0.01f * f,
                                     0.6f, 1.0f + 0.05f * f));
    }
    water.contributors.emplace_back(crowded);

    GerstnerContributor still;   // nothing in it moves the surface
    still.waves = {wave({0.0f, 0.0f}, 9.0f, 3.0f, 0.5f)};
    water.contributors.emplace_back(still);

    for (int i = 0; i < 3; ++i) {
        GerstnerContributor more;
        more.speed_scale = 1.0f + 0.3f * static_cast<float>(i);
        more.waves = {wave({-1.0f, 0.2f * static_cast<float>(i)}, 40.0f, 0.8f, 0.7f)};
        water.contributors.emplace_back(more);
    }

    auto packed = to_gpu_field(water);
    REQUIRE(packed.gerstner_count == MAX_GERSTNER_CONTRIBUTORS);
    REQUIRE(packed.gerstner[0].wave_count == 2);
    REQUIRE(packed.gerstner[0].waves[1].wavelength == Approx(13.0f));
    REQUIRE(packed.gerstner[1].wave_count == MAX_WATER_WAVES);
    REQUIRE(packed.gerstner[2].wave_count == 0);
    REQUIRE(packed.gerstner[3].wave_count == 1);

    FjellWaterField shader_field = as_shader_reads(water);
    for (float time : {0.0f, 3.0f, 77.5f}) {
        for (float x = -30.0f; x <= 30.0f; x += 6.0f) {
            for (float z = -30.0f; z <= 30.0f; z += 10.0f) {
                glm::vec2 p(x, z);
                INFO("at (" << x << ", " << z << ") t=" << time);
                glm::vec3 cpu_d = water::displace(run(water), p, time);
                vec3 gpu_d = fjell_water_displace(shader_field, p, time);
                REQUIRE(cpu_d.x == Approx(gpu_d.x).margin(1e-3));
                REQUIRE(cpu_d.y == Approx(gpu_d.y).margin(1e-3));
                REQUIRE(cpu_d.z == Approx(gpu_d.z).margin(1e-3));

                glm::vec3 cpu_n = water::normal_at(run(water), p, time);
                vec3 gpu_n = fjell_water_normal(shader_field, p, time);
                REQUIRE(cpu_n.x == Approx(gpu_n.x).margin(1e-2));
                REQUIRE(cpu_n.y == Approx(gpu_n.y).margin(1e-2));
                REQUIRE(cpu_n.z == Approx(gpu_n.z).margin(1e-2));

                REQUIRE(water::horizontal_jacobian(run(water), p, time)
                        == Approx(fjell_water_jacobian(shader_field, p, time)).margin(1e-3));
            }
        }
    }
}

// --- A simulated contributor beside the authored ones ---

TEST_CASE("Physics does not see the simulated sea", "[fjwater]") {
    // The simulation lives on the GPU, so the CPU evaluator sums the
    // closed-form contributors and nothing else: a field with a simulated
    // contributor in it floats a boat exactly where the same field without it
    // does. That is a decision with a known cost, and this is what holds the
    // evaluator to it until the day it is changed on purpose.
    FjWaterData authored = sea({
        wave({1.0f, 0.2f}, 40.0f, 1.2f, 0.7f),
        wave({-0.3f, 1.0f}, 9.0f, 0.3f, 0.8f, 1.2f),
    });
    FjWaterData with_simulation = authored;
    with_simulation.contributors.insert(with_simulation.contributors.begin(),
                                        FftContributor{});

    for (float x = -20.0f; x <= 20.0f; x += 5.0f) {
        glm::vec2 p(x, 4.0f - x * 0.5f);
        REQUIRE(water::displace(run(with_simulation), p, 2.5f) == water::displace(run(authored), p, 2.5f));
        REQUIRE(water::normal_at(run(with_simulation), p, 2.5f) == water::normal_at(run(authored), p, 2.5f));
        REQUIRE(water::horizontal_jacobian(run(with_simulation), p, 2.5f)
                == water::horizontal_jacobian(run(authored), p, 2.5f));
    }
    REQUIRE(water::bed_agitation(with_simulation, 3.0f) == water::bed_agitation(authored, 3.0f));

    // The shader is told where the simulated sea tiles, and nothing else of
    // it goes through the field buffer.
    FftContributor fft;
    fft.patch_size = 180.0f;
    with_simulation.contributors.front() = fft;
    REQUIRE(to_gpu_field(with_simulation).simulated_patch_size == 180.0f);
    REQUIRE(to_gpu_field(with_simulation).gerstner_count == 1);
    REQUIRE(to_gpu_field(authored).simulated_patch_size == 0.0f);
}

TEST_CASE("A simulated contributor survives a round trip beside an authored one", "[fjwater]") {
    auto dir = std::filesystem::temp_directory_path() / "fjell-fjwater-fft";
    std::filesystem::create_directories(dir);
    auto path = (dir / "mixed.fjwater").string();

    FftContributor fft;
    fft.wind_direction = {0.6f, -0.8f};
    fft.wind_speed = 13.5f;
    fft.answers_weather = true;
    fft.fetch = 42000.0f;
    fft.spread_degrees = 18.0f;
    fft.high_cut = 6.0f;
    fft.choppiness = 1.4f;
    fft.amplitude_scale = 0.75f;
    fft.patch_size = 320.0f;
    fft.resolution = 512;
    fft.seed = 99;

    FjWaterData written = sea({wave({1.0f, 0.0f}, 70.0f, 1.0f, 0.5f)});
    written.contributors.emplace_back(fft);
    REQUIRE(save_fjwater_file(path, written));

    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(loaded->contributors.size() == 2);
    REQUIRE(std::holds_alternative<GerstnerContributor>(loaded->contributors[0]));
    REQUIRE(std::get<FftContributor>(loaded->contributors[1]) == fft);

    std::filesystem::remove_all(dir);
}

TEST_CASE("A simulated sea answers the weather only when told to", "[fjwater]") {
    FftContributor authored;
    authored.wind_direction = {1.0f, 0.0f};
    authored.wind_speed = 8.0f;
    FjWaterData water;
    water.contributors.emplace_back(authored);

    const glm::vec2 gale{0.0f, 20.0f};
    REQUIRE(std::get<FftContributor>(water::apply_wind(water, gale).contributors[0]) == authored);

    std::get<FftContributor>(water.contributors[0]).answers_weather = true;
    const auto blown = std::get<FftContributor>(water::apply_wind(water, gale).contributors[0]);
    REQUIRE(blown.wind_speed == Approx(20.0f));
    REQUIRE(blown.wind_direction.x == Approx(0.0f).margin(1e-6));
    REQUIRE(blown.wind_direction.y == Approx(1.0f));
    // Everything that is not the wind is the author's, untouched.
    FftContributor same_wind = blown;
    same_wind.wind_speed = authored.wind_speed;
    same_wind.wind_direction = authored.wind_direction;
    same_wind.answers_weather = false;
    REQUIRE(same_wind == authored);

    // More wind is a bigger sea, which is the spectrum's doing and not a dial's.
    REQUIRE(water::wave_height(water::apply_wind(water, gale))
            > water::wave_height(water::apply_wind(water, {0.0f, 6.0f})));

    // A calm flattens it and leaves it facing the way it was, so there is a
    // heading for the wind to come back to and never a NaN in one.
    const auto calm = std::get<FftContributor>(
        water::apply_wind(water, {0.0f, 0.0f}).contributors[0]);
    REQUIRE(calm.wind_speed == Approx(0.0f));
    REQUIRE(calm.wind_direction == authored.wind_direction);
    REQUIRE(water::wave_height(water::apply_wind(water, {0.0f, 0.0f})) == Approx(0.0f));
}

// --- A body that feels the simulated sea ---

namespace {

/// A swell with a simulated sea under it, asking physics to feel `waves` of it.
[[nodiscard]] FjWaterData sea_physics_feels(std::uint32_t waves) {
    FftContributor fft;
    fft.wind_direction = {0.8f, 0.6f};
    fft.wind_speed = 10.0f;
    fft.fetch = 80000.0f;
    fft.spread_degrees = 35.0f;
    fft.high_cut = 0.0f;
    fft.choppiness = 1.4f;
    fft.amplitude_scale = 1.0f;
    fft.patch_size = 128.0f;
    fft.resolution = 64;
    fft.physics_waves = waves;
    FjWaterData water = sea({wave({0.2f, 1.0f}, 70.0f, 0.9f, 0.5f)});
    water.contributors.emplace_back(fft);
    return water;
}

} // namespace

TEST_CASE("Physics feels the simulated sea when it is asked to", "[fjwater]") {
    const FjWaterData authored = sea_physics_feels(48);
    // A field straight from a file has no derived waves, and floats a body on
    // the authored contributors alone.
    REQUIRE_FALSE(authored.simulated.has_value());
    // Answering the wind alone draws nothing either.
    REQUIRE_FALSE(water::apply_wind(authored, {0.0f, 0.0f}).simulated.has_value());

    const FjWaterData effective = water::derive_effective(authored, {0.0f, 0.0f});
    REQUIRE(effective.simulated.has_value());
    REQUIRE(effective.simulated->spectrum != nullptr);
    REQUIRE(effective.simulated->spectrum->resolution == 64);
    REQUIRE(effective.simulated->waves.size() == 48);
    REQUIRE(effective.simulated->choppiness == Approx(1.4f));

    // Asked for none, the sea is still drawn — the renderer animates it — but
    // physics is given no waves, and the evaluator is what it was.
    const FjWaterData unfelt = water::derive_effective(sea_physics_feels(0), {0.0f, 0.0f});
    REQUIRE(unfelt.simulated.has_value());
    REQUIRE(unfelt.simulated->waves.empty());

    bool moved = false;
    for (float x = -30.0f; x <= 30.0f; x += 7.0f) {
        const glm::vec2 p(x, 5.0f - x * 0.3f);
        const float time = 4.5f;
        INFO("at x=" << x);

        // The surface is the authored one plus the waves, and nothing else.
        const glm::vec3 d = water::displace(effective, p, time);
        const glm::vec3 expected = water::displace(run(unfelt), p, time)
            + water::spectral_displacement(effective.simulated->waves, 1.4f, p, time);
        REQUIRE(d.x == Approx(expected.x).margin(1e-5));
        REQUIRE(d.y == Approx(expected.y).margin(1e-5));
        REQUIRE(d.z == Approx(expected.z).margin(1e-5));
        if (glm::length(d - water::displace(run(unfelt), p, time)) > 0.05f) moved = true;

        // The normal and the Jacobian are assembled from parts, and the parts
        // could be put together wrongly while each is right. What they have to
        // agree with is the displaced surface itself, measured a few
        // centimetres either side — arithmetic, not another formula of mine.
        const float step = 0.02f;
        auto surface = [&](glm::vec2 rest) {
            const glm::vec3 moved_by = water::displace(effective, rest, time);
            return glm::vec3(rest.x + moved_by.x, moved_by.y, rest.y + moved_by.z);
        };
        const glm::vec3 along_x = (surface(p + glm::vec2(step, 0)) - surface(p - glm::vec2(step, 0)))
                                / (2.0f * step);
        const glm::vec3 along_z = (surface(p + glm::vec2(0, step)) - surface(p - glm::vec2(0, step)))
                                / (2.0f * step);
        const glm::vec3 measured_normal = glm::normalize(glm::cross(along_z, along_x));
        const glm::vec3 n = water::normal_at(effective, p, time);
        REQUIRE(n.x == Approx(measured_normal.x).margin(3e-3));
        REQUIRE(n.y == Approx(measured_normal.y).margin(3e-3));
        REQUIRE(n.z == Approx(measured_normal.z).margin(3e-3));

        const float measured_jacobian = along_x.x * along_z.z - along_x.z * along_z.x;
        REQUIRE(water::horizontal_jacobian(effective, p, time)
                == Approx(measured_jacobian).margin(3e-3));

        // And a body asking for the water over a column lands on it.
        const glm::vec2 rest = water::rest_position_for(effective, p, time);
        const glm::vec3 there = water::displace(effective, rest, time);
        REQUIRE(rest.x + there.x == Approx(p.x).margin(1e-2));
        REQUIRE(rest.y + there.z == Approx(p.y).margin(1e-2));
    }
    REQUIRE(moved);   // the waves are not nothing
}

TEST_CASE("A drawn sea is kept while it is the same sea", "[fjwater]") {
    const FjWaterData authored = sea_physics_feels(32);
    const FjWaterData first = water::derive_effective(authored, {0.0f, 0.0f});

    // Handed back, the sea is kept rather than drawn again: the very same
    // spectrum, not an equal one.
    const FjWaterData again = water::derive_effective(authored, {0.0f, 0.0f}, &first);
    REQUIRE(again.simulated->spectrum == first.simulated->spectrum);

    // Choppiness and the count physics feels are not the spectrum: the sea
    // stands, and they follow.
    FjWaterData retuned = authored;
    std::get<FftContributor>(retuned.contributors[1]).choppiness = 2.5f;
    std::get<FftContributor>(retuned.contributors[1]).physics_waves = 40;
    const FjWaterData kept = water::derive_effective(retuned, {0.0f, 0.0f}, &first);
    REQUIRE(kept.simulated->spectrum == first.simulated->spectrum);
    REQUIRE(kept.simulated->choppiness == Approx(2.5f));
    REQUIRE(kept.simulated->waves.size() == 40);
    // The longer list starts with the shorter one: the same strongest waves.
    for (std::size_t i = 0; i < first.simulated->waves.size(); ++i) {
        REQUIRE(kept.simulated->waves[i].k == first.simulated->waves[i].k);
    }

    // A different sea is drawn afresh.
    FjWaterData windier = authored;
    std::get<FftContributor>(windier.contributors[1]).wind_speed = 14.0f;
    REQUIRE(water::derive_effective(windier, {0.0f, 0.0f}, &first).simulated->spectrum
            != first.simulated->spectrum);
}

TEST_CASE("One draw feeds the renderer and physics alike", "[fjwater]") {
    // The point of one owner: the waves a boat floats on are the strongest
    // wavevectors of the very spectrum the GPU is handed, so the two cannot be
    // different seas. Held to the spectrum itself rather than to the function
    // that took them.
    const FjWaterData effective = water::derive_effective(sea_physics_feels(24), {0.0f, 0.0f});
    const auto& sea = *effective.simulated;
    const std::uint32_t n = sea.spectrum->resolution;
    for (std::size_t i = 0; i < sea.waves.size(); ++i) {
        const SpectralWave& wave = sea.waves[i];
        const auto x = static_cast<std::uint32_t>(
            (static_cast<int>(std::lround(wave.k.x / sea.spectrum->k_spacing)) + static_cast<int>(n)) % static_cast<int>(n));
        const auto z = static_cast<std::uint32_t>(
            (static_cast<int>(std::lround(wave.k.y / sea.spectrum->k_spacing)) + static_cast<int>(n)) % static_cast<int>(n));
        const glm::vec2 amplitude = sea.spectrum->amplitudes[z * n + x];
        // The stronger half is the spectrum's own; the weaker half is tapered
        // toward the cutoff, and never more than the spectrum's.
        if (i < sea.waves.size() / 2) {
            REQUIRE(wave.amplitude == Approx(2.0f * glm::length(amplitude)).epsilon(1e-5));
        } else {
            REQUIRE(wave.amplitude <= 2.0f * glm::length(amplitude) * (1.0f + 1e-5f));
        }
    }
}

TEST_CASE("A sea being reshaped goes on showing the last one, then changes once", "[fjwater]") {
    // What the per-frame derivation adds to the one-off: a new sea is shaped on
    // a worker while the last one stays in use, and both halves — the spectrum
    // the renderer uploads and the waves physics sums — change together, in
    // the frame the worker is done. Never a frame with one new and one old.
    ftk::ThreadPool workers(2);
    water::SeaDerivation derivation;
    FjWaterData authored = sea_physics_feels(16);

    const FjWaterData first = derivation.derive(authored, {0.0f, 0.0f}, &workers);
    REQUIRE(first.simulated.has_value());
    REQUIRE(first.simulated->waves.size() == 16);

    std::get<FftContributor>(authored.contributors[1]).wind_speed = 17.0f;
    bool changed = false;
    for (int frame = 0; frame < 2000 && !changed; ++frame) {
        const FjWaterData now = derivation.derive(authored, {0.0f, 0.0f}, &workers);
        REQUIRE(now.simulated.has_value());
        const bool new_spectrum = now.simulated->spectrum != first.simulated->spectrum;
        const bool new_waves = now.simulated->waves[0].amplitude != first.simulated->waves[0].amplitude;
        INFO("frame " << frame);
        REQUIRE(new_spectrum == new_waves);   // together or not at all
        if (new_spectrum) {
            REQUIRE(now.simulated->drawn_from.wind_speed == Approx(17.0f));
            changed = true;
        } else {
            REQUIRE(now.simulated->drawn_from.wind_speed == Approx(10.0f));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    REQUIRE(changed);

    // What the worker shaped is what drawing it here would have given.
    const FjWaterData reshaped = derivation.derive(authored, {0.0f, 0.0f}, &workers);
    const FjWaterData direct = water::derive_effective(authored, {0.0f, 0.0f});
    REQUIRE(reshaped.simulated->spectrum->amplitudes == direct.simulated->spectrum->amplitudes);

    // A change of grid has nothing of the old sea to keep showing, and is
    // drawn at once; so is anything when there are no workers.
    std::get<FftContributor>(authored.contributors[1]).patch_size = 200.0f;
    const FjWaterData regridded = derivation.derive(authored, {0.0f, 0.0f}, &workers);
    REQUIRE(regridded.simulated->drawn_from.patch_size == Approx(200.0f));
    std::get<FftContributor>(authored.contributors[1]).wind_speed = 6.0f;
    REQUIRE(derivation.derive(authored, {0.0f, 0.0f}, nullptr).simulated->drawn_from.wind_speed
            == Approx(6.0f));

    // No simulated contributor, no sea.
    REQUIRE_FALSE(derivation.derive(sea({wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.5f)}), {0.0f, 0.0f},
                                    &workers).simulated.has_value());
}

TEST_CASE("A sea under the weather changes in steps", "[fjwater]") {
    // The lagged wind drifts every frame. Winds a hair apart have to be the
    // same sea, or it is drawn again every frame, and have to be the same sea
    // on every machine given that wind, or boats float differently on each.
    FjWaterData water = sea_physics_feels(16);
    std::get<FftContributor>(water.contributors[1]).answers_weather = true;

    auto blown = [&](glm::vec2 wind) {
        return std::get<FftContributor>(water::apply_wind(water, wind).contributors[1]);
    };
    // Both within the step that rounds to 12.3, which runs from 12.25 to 12.35.
    REQUIRE(blown({12.27f, 0.0f}) == blown({12.33f, 0.0f}));
    REQUIRE(blown({12.30f, 0.0f}) == blown({12.30f, 0.02f}));
    REQUIRE(blown({12.27f, 0.0f}).wind_speed == Approx(12.3f));
    // Either side of 12.35 are different seas.
    REQUIRE(blown({12.33f, 0.0f}) != blown({12.37f, 0.0f}));
    REQUIRE(blown({0.0f, 12.0f}).wind_direction.y == Approx(1.0f));

    // A round trip keeps the count.
    auto path = (std::filesystem::temp_directory_path() / "fjell_physics_waves.fjwater").string();
    REQUIRE(save_fjwater_file(path, water));
    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(std::get<FftContributor>(loaded->contributors[1]).physics_waves == 16);
    REQUIRE_FALSE(loaded->simulated.has_value());
    std::filesystem::remove(path);
}

TEST_CASE("The surface's velocity is the derivative of where it is", "[fjwater]") {
    // surface_motion() gives the water's velocity from the same sines and
    // cosines as its displacement. What it has to agree with is the
    // displacement itself, a moment earlier and later: a central difference
    // over a shrinking step converges on the derivative, and if it did not
    // converge on this velocity the velocity would be wrong. Both halves of
    // the field are in play, the trains and the simulated waves, on a sea
    // with the pinch turned well up so the horizontal terms are not small.
    FjWaterData authored = sea_physics_feels(40);
    std::get<GerstnerContributor>(authored.contributors[0]).waves.push_back(
        wave({-0.6f, 0.8f}, 23.0f, 0.7f, 0.9f, 1.3f));
    std::get<FftContributor>(authored.contributors[1]).choppiness = 2.5f;
    const FjWaterData water = water::derive_effective(authored, {0.0f, 0.0f});
    REQUIRE(water.simulated->waves.size() == 40);

    for (float time : {0.0f, 2.7f, 31.0f}) {
        for (float x = -25.0f; x <= 25.0f; x += 5.0f) {
            const glm::vec2 p(x, 3.0f - 0.4f * x);
            const water::SurfaceMotion m = water::surface_motion(run(water), p, time);
            INFO("at x=" << x << " t=" << time);

            // The displacement is displace(), exactly: the same sums.
            REQUIRE(m.displacement == water::displace(run(water), p, time));

            // A third of the step must cut the disagreement with the
            // difference by well over half, which is what a derivative means;
            // a wrong sign or a missing omega on any term would leave the
            // error where it is. The steps stay above a few milliseconds:
            // below that the difference of two float displacements a few
            // metres tall is rounding, not slope — measured, the error turns
            // and grows again under about 3e-3.
            auto difference = [&](float h) {
                return (water::displace(run(water), p, time + h) - water::displace(run(water), p, time - h))
                     / (2.0f * h);
            };
            const float coarse = glm::length(difference(3e-2f) - m.velocity);
            const float fine = glm::length(difference(1e-2f) - m.velocity);
            REQUIRE(fine <= 5e-4f);
            REQUIRE(fine < coarse * 0.5f);
        }
    }

    // What a body used to be given: the average velocity over the tick it just
    // had. The exact velocity sits within a tick's acceleration of it.
    const float dt = 1.0f / 60.0f;
    const glm::vec2 p(4.0f, -7.0f);
    const glm::vec3 old_flow = (water::displace(run(water), p, 5.0f) - water::displace(run(water), p, 5.0f - dt)) / dt;
    REQUIRE(glm::length(water::surface_motion(run(water), p, 5.0f).velocity - old_flow) < 0.15f);
}

TEST_CASE("A derived sea's packed waves are its waves", "[fjwater]") {
    // The evaluators read the packed copy; everything else reads the list.
    // retune() is the one place that sets either, so a change of count must
    // leave them the same waves.
    FjWaterData authored = sea_physics_feels(20);
    FjWaterData eff = water::derive_effective(authored, {0.0f, 0.0f});
    auto in_step = [](const FjWaterData::SimulatedSea& s) {
        REQUIRE(s.packed.count == s.waves.size());
        for (std::size_t i = 0; i < s.waves.size(); ++i) {
            REQUIRE(s.packed.kx[i] == s.waves[i].k.x);
            REQUIRE(s.packed.amplitude[i] == s.waves[i].amplitude);
            REQUIRE(s.packed.phase[i] == s.waves[i].phase);
        }
        for (std::size_t i = s.waves.size(); i < s.packed.kx.size(); ++i) {
            REQUIRE(s.packed.amplitude[i] == 0.0f);
        }
    };
    in_step(*eff.simulated);
    std::get<FftContributor>(authored.contributors[1]).physics_waves = 37;
    eff = water::derive_effective(authored, {0.0f, 0.0f}, &eff);
    REQUIRE(eff.simulated->waves.size() == 37);
    in_step(*eff.simulated);
}

TEST_CASE("The world's field carries its trains prepared", "[fjwater]") {
    // Every field the engine evaluates comes through apply_wind(), and it is
    // apply_wind() that prepares the trains the point evaluators read. What
    // that has to give is exactly the trains for_each_summed_train() visits,
    // in its order, padded to eight.
    FjWaterData raw = sea({
        wave({1.0f, 0.3f}, 28.0f, 1.4f, 0.85f, 1.0f),
        wave({0.0f, 0.0f}, 20.0f, 1.0f, 0.5f),    // no direction: not summed, not prepared
        wave({-0.4f, 1.0f}, 13.0f, 0.6f, 0.7f, 1.3f),
    });
    raw.contributors.emplace_back(GerstnerContributor{});
    gerstner(raw, 1).waves = {wave({0.2f, 1.0f}, 60.0f, 1.0f, 0.4f)};
    gerstner(raw, 1).amplitude_scale = 1.7f;
    REQUIRE_FALSE(raw.packed_trains.has_value());

    const FjWaterData prepared = water::apply_wind(raw, {0.0f, 0.0f});
    REQUIRE(prepared.packed_trains.has_value());
    const auto& t = *prepared.packed_trains;
    REQUIRE(t.count == 3);
    REQUIRE(t.k.size() == 8);
    // The third summed train is the second contributor's, under its scale.
    REQUIRE(t.amplitude[2] == Approx(1.7f));
    REQUIRE(t.k[2] == Approx(glm::two_pi<float>() / 60.0f));
    REQUIRE(glm::length(glm::vec2(t.dir_x[0], t.dir_z[0])) == Approx(1.0f));
    for (std::size_t i = 3; i < 8; ++i) REQUIRE(t.amplitude[i] == 0.0f);
}

// --- Auditing the waves physics feels, 0 to 256 ---
//
// Kay's tuned sea, written out rather than read from the project so the
// numbers below mean the same thing on any machine: 9 m/s over 60 km,
// choppiness 3, height x2.9, a 1.6 m high cut, 60 degrees of spread, 256 m
// at 256. It folds about a sixth of its surface.

namespace {

[[nodiscard]] FjWaterData tuned_sea(std::uint32_t physics_waves, bool answers_weather = false) {
    FftContributor fft;
    fft.wind_direction = {0.99f, 0.12f};
    fft.wind_speed = 9.0f;
    fft.answers_weather = answers_weather;
    fft.fetch = 60000.0f;
    fft.spread_degrees = 60.0f;
    fft.high_cut = 1.6f;
    fft.choppiness = 3.0f;
    fft.amplitude_scale = 2.42f;
    fft.patch_size = 256.0f;
    fft.resolution = 256;
    fft.seed = 1;
    fft.physics_waves = physics_waves;
    FjWaterData water;
    water.contributors.emplace_back(fft);
    return water;
}

/// The whole simulated surface's height at every grid point, by the
/// transform: the sea as drawn, standing Nyquist ripples and all.
[[nodiscard]] std::vector<float> full_height(const FjWaterData::SimulatedSea& sea, float time) {
    return water::inverse_transform(
        water::evolve_spectrum(*sea.spectrum, sea.choppiness, time).height,
        sea.spectrum->resolution);
}

[[nodiscard]] double rms(const std::vector<float>& v) {
    double s = 0.0;
    for (float x : v) s += static_cast<double>(x) * x;
    return std::sqrt(s / static_cast<double>(v.size()));
}

} // namespace

TEST_CASE("The readout's share of the height is the surface's", "[fjwater][audit]") {
    // "feels N% of its height" is worked out from the spectrum. What it claims
    // is a fact about the surface: the RMS height of the N-wave sea over the
    // RMS height of the whole sea, averaged over time. Over time, because the
    // waves at k and -k share a spatial frequency and travel opposite ways, so
    // at any one instant they interfere and the surface's share sits a point
    // or so either side of the average — measured 88.4% to 90.3% around a
    // claimed 89.1% at 256 waves. Averaged over instants the interference
    // cancels and Parseval holds, so an error in the claim would show as a
    // mean that drifts from it.
    const FjWaterData whole = water::derive_effective(tuned_sea(0), {0.0f, 0.0f});
    const auto& sea = *whole.simulated;
    const std::uint32_t n = sea.spectrum->resolution;
    const float texel = 256.0f / static_cast<float>(n);
    const float instants[] = {0.0f, 5.0f, 37.0f, 111.0f, 250.0f, 613.0f, 1000.0f, 1777.0f};

    for (std::uint32_t count : {22u, 64u, 128u, 256u}) {
        const water::StrongestWaves taken = water::strongest_waves(*sea.spectrum, count);
        const FjWaterData felt = water::derive_effective(tuned_sea(count), {0.0f, 0.0f});
        double partial_ms = 0.0, full_ms = 0.0;
        for (float time : instants) {
            const std::vector<float> full = full_height(sea, time);
            for (std::uint32_t z = 0; z < n; z += 2) {
                for (std::uint32_t x = 0; x < n; x += 2) {
                    const double h = water::displace(
                        felt, {static_cast<float>(x) * texel, static_cast<float>(z) * texel}, time).y;
                    partial_ms += h * h;
                    full_ms += static_cast<double>(full[z * n + x]) * full[z * n + x];
                }
            }
        }
        const double measured = std::sqrt(partial_ms / full_ms);
        WARN(count << " waves: readout says " << taken.height_share * 100.0f
             << "% of the height, the surface measures " << measured * 100.0
             << "%, shortest wave " << taken.shortest_wavelength << " m");
        REQUIRE(measured == Approx(taken.height_share).margin(0.01));
    }
}

TEST_CASE("The rest-position solve converges on a folded sea", "[fjwater][audit]") {
    // A body over a column asks which rest position stands over it. The
    // solve is Newton with eight steps and a millimetre tolerance, tuned on
    // Gerstner seas. On this sea at 256 waves and choppiness 3 the pinch is
    // large and a sixth of the surface has folded, where the Jacobian goes
    // through zero and no single rest position exists. What has to hold is
    // that the solve lands on the surface — the residual it was asked for —
    // wherever the surface has not folded; over a fold, any branch it lands on
    // is real surface, and what is reported is how often it lands at all.
    //
    // Two things are required, both properties of a bounded solve rather than
    // numbers picked here: it never ends further off than it began, and a
    // miss is never further than one step, since a step is held to half the
    // shortest wave.
    const FjWaterData felt = water::derive_effective(tuned_sea(256), {0.0f, 0.0f});
    int unfolded = 0, unfolded_missed = 0, folded = 0, folded_missed = 0, ran_away = 0;
    float worst_unfolded = 0.0f;
    for (float time : {0.0f, 3.3f, 17.9f}) {
        for (float x = -120.0f; x <= 120.0f; x += 8.0f) {
            for (float z = -120.0f; z <= 120.0f; z += 8.0f) {
                const glm::vec2 target(x, z);
                const glm::vec2 rest = water::rest_position_for(felt, target, time);
                const glm::vec3 d = water::displace(felt, rest, time);
                const float residual = glm::length(glm::vec2(rest.x + d.x, rest.y + d.z) - target);
                // Where the solve would have started: the residual of taking
                // the column itself as the rest position. A miss larger than
                // that is Newton having made things worse, not stopped short.
                const glm::vec3 at_start = water::displace(felt, target, time);
                const float start_residual = glm::length(glm::vec2(at_start.x, at_start.z));
                if (residual > start_residual) ++ran_away;
                // Folded where the surface, at the rest position found, has
                // pinched to a point or through itself.
                const bool over_fold = water::horizontal_jacobian(felt, rest, time) <= 0.1f;
                if (over_fold) {
                    ++folded;
                    if (residual > 1e-2f) ++folded_missed;
                } else {
                    ++unfolded;
                    worst_unfolded = std::max(worst_unfolded, residual);
                    if (residual > 1e-2f) ++unfolded_missed;
                }
            }
        }
    }
    WARN("rest-position solve at 256 waves: " << unfolded << " unfolded columns, "
         << unfolded_missed << " missed by over a centimetre (worst " << worst_unfolded
         << " m); " << folded << " folded columns, " << folded_missed << " not landed; "
         << ran_away << " columns where the solve ended further off than it started");
    REQUIRE(ran_away == 0);
    REQUIRE(worst_unfolded < water::shortest_wavelength(felt) * 0.5f);
}

TEST_CASE("A wind step does not jolt a body more than it moves the sea", "[fjwater][audit]") {
    // Under the weather the sea is redrawn each time the wind crosses a step
    // of 0.1 m/s, and the N strongest waves are picked again. A wave near the
    // cutoff can enter or leave the set, so the surface a body feels can jump
    // where the whole sea only shifted a little. The change the whole sea
    // makes over the step is the yardstick: the N-wave surface's jump is held
    // to a small multiple of it, and the ratio is reported for every count.
    // The multiple, three, is a judgement and not a derivation — a jolt of the
    // same order as the sea's own change is one a body would not notice, and
    // with the weaker half of the set tapered the smallest count measures a
    // little over two.
    const float time = 4.0f;
    for (std::uint32_t count : {22u, 64u, 128u, 256u}) {
        const FjWaterData before = water::derive_effective(tuned_sea(count, true), {9.0f, 0.0f});
        const FjWaterData after = water::derive_effective(tuned_sea(count, true), {9.1f, 0.0f});
        REQUIRE(before.simulated->spectrum != after.simulated->spectrum);   // a real reshape

        const std::vector<float> full_a = full_height(*before.simulated, time);
        const std::vector<float> full_b = full_height(*after.simulated, time);
        std::vector<float> full_delta(full_a.size());
        for (std::size_t i = 0; i < full_a.size(); ++i) full_delta[i] = full_b[i] - full_a[i];

        const std::uint32_t n = before.simulated->spectrum->resolution;
        const float texel = 256.0f / static_cast<float>(n);
        std::vector<float> felt_delta(full_a.size());
        for (std::uint32_t z = 0; z < n; z += 4) {
            for (std::uint32_t x = 0; x < n; x += 4) {
                const glm::vec2 p{static_cast<float>(x) * texel, static_cast<float>(z) * texel};
                felt_delta[z * n + x] = water::displace(after, p, time).y - water::displace(before, p, time).y;
            }
        }
        // Both over the same sample of points.
        std::vector<float> full_sampled, felt_sampled;
        for (std::uint32_t z = 0; z < n; z += 4)
            for (std::uint32_t x = 0; x < n; x += 4) {
                full_sampled.push_back(full_delta[z * n + x]);
                felt_sampled.push_back(felt_delta[z * n + x]);
            }
        const double sea_moved = rms(full_sampled);
        const double body_felt = rms(felt_sampled);
        WARN(count << " waves: the wind step moved the sea by " << sea_moved * 1000.0
             << " mm RMS and the surface physics feels by " << body_felt * 1000.0
             << " mm RMS (ratio " << body_felt / sea_moved << ")");
        CHECK(body_felt < 3.0 * sea_moved + 1e-3);
    }
}
