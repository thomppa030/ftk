#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/fjwater_io.hpp"
#include "renderer/resources/water_gpu.hpp"
#include "renderer/resources/water_surface.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// The shader half of the wave field, compiled as C++ so the two evaluators
// can be compared directly. See glsl_shim.hpp for why this works and what it
// is limited to.
#include "glsl_shim.hpp"
#include "../shaders/include/fjell_water_waves.glsl"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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

    pair.gpu.wave_count = static_cast<unsigned int>(waves.size());
    pair.gpu.amplitude_scale = amplitude_scale;
    pair.gpu.speed_scale = speed_scale;
    pair.gpu.steepness_headroom = gerstner(pair.cpu).steepness_headroom;
    for (std::size_t i = 0; i < waves.size() && i < MAX_WATER_WAVES; ++i) {
        pair.gpu.waves[i].direction = waves[i].direction;
        pair.gpu.waves[i].wavelength = waves[i].wavelength;
        pair.gpu.waves[i].amplitude = waves[i].amplitude;
        pair.gpu.waves[i].steepness = waves[i].steepness;
        pair.gpu.waves[i].speed = waves[i].speed;
    }
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
                glm::vec3 cpu_d = water::displace(field.cpu, p, time);
                vec3 gpu_d = fjell_water_displace(field.gpu, p, time);
                REQUIRE(cpu_d.x == Approx(gpu_d.x).margin(1e-3));
                REQUIRE(cpu_d.y == Approx(gpu_d.y).margin(1e-3));
                REQUIRE(cpu_d.z == Approx(gpu_d.z).margin(1e-3));

                glm::vec3 cpu_n = water::normal_at(field.cpu, p, time);
                vec3 gpu_n = fjell_water_normal(field.gpu, p, time);
                REQUIRE(cpu_n.x == Approx(gpu_n.x).margin(1e-2));
                REQUIRE(cpu_n.y == Approx(gpu_n.y).margin(1e-2));
                REQUIRE(cpu_n.z == Approx(gpu_n.z).margin(1e-2));

                REQUIRE(water::crest(field.cpu, p, time)
                        == Approx(fjell_water_crest(field.gpu, p, time)).margin(1e-3));
            }
        }
    }

    // Not a function of position or time, so it is checked once rather than
    // over the grid — but it is half of a pair like the rest, and the pass
    // hands the CPU answer to the shader through a push constant, so a drift
    // here would point foam the wrong way with nothing to catch it.
    glm::vec2 cpu_dir = water::dominant_direction(field.cpu);
    vec2 gpu_dir = fjell_water_dominant_dir(field.gpu);
    REQUIRE(cpu_dir.x == Approx(gpu_dir.x).margin(1e-4));
    REQUIRE(cpu_dir.y == Approx(gpu_dir.y).margin(1e-4));
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
            REQUIRE(water::displace(field.cpu, p, time).y
                    == Approx(fjell_water_displace(field.gpu, p, time).y).margin(1e-3));
            REQUIRE(water::crest(field.cpu, p, time)
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
        REQUIRE(water::displace(field.cpu, p, 1.0f).y
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
                REQUIRE(water::displace(split, p, time) == water::displace(whole, p, time));
                REQUIRE(water::normal_at(split, p, time) == water::normal_at(whole, p, time));
                REQUIRE(water::horizontal_jacobian(split, p, time)
                        == water::horizontal_jacobian(whole, p, time));
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
            glm::vec3 together = water::displace(both, p, time);
            glm::vec3 apart = water::displace(swell, p, time) + water::displace(chop, p, time);
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
    REQUIRE(water::displace(over, p, 1.5f) == water::displace(water, p, 1.5f));
    REQUIRE(water::wave_height(over) == Approx(water::wave_height(water)));
    REQUIRE(water::shortest_wavelength(over) == Approx(20.0f));
}

// --- The field behaves like water ---

TEST_CASE("Flat water is flat", "[fjwater]") {
    FjWaterData water;  // no waves
    REQUIRE(water::height_at(water, {3.0f, 7.0f}, 5.0f) == Approx(0.0f));
    REQUIRE(water::normal_at(water, {3.0f, 7.0f}, 5.0f).y == Approx(1.0f));
    REQUIRE(water::crest(water, {3.0f, 7.0f}, 5.0f) == Approx(0.0f));
}

TEST_CASE("A wave stays within its amplitude", "[fjwater]") {
    FjWaterData water = sea();
    gerstner(water).waves = {wave({1.0f, 0.0f}, 20.0f, 1.5f, 0.9f)};

    float highest = -100.0f;
    float lowest = 100.0f;
    for (float x = 0.0f; x < 40.0f; x += 0.05f) {
        float h = water::height_at(water, {x, 0.0f}, 0.0f);
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
            peak = std::max(peak, water::crest(water, {x, 0.0f}, 0.0f));
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
        worst = std::min(worst, water::horizontal_jacobian(water, {x, 0.0f}, 0.0f));
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
        worst = std::min(worst, water::horizontal_jacobian(water, {x, 0.0f}, 0.0f));
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
    field.gpu.steepness_headroom = 3.0f;

    for (float time : {0.0f, 1.7f, 9.25f}) {
        for (float x = -20.0f; x <= 20.0f; x += 2.5f) {
            for (float z = -20.0f; z <= 20.0f; z += 2.5f) {
                glm::vec2 p{x, z};
                REQUIRE(water::horizontal_jacobian(field.cpu, p, time)
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
        glm::vec2 rest = water::rest_position_for(water, target, 2.0f);
        glm::vec3 d = water::displace(water, rest, 2.0f);

        REQUIRE(rest.x + d.x == Approx(target.x).margin(1e-2));
        REQUIRE(rest.y + d.z == Approx(target.y).margin(1e-2));
        REQUIRE(d.y == Approx(water::height_at_iterative(water, target, 2.0f)).margin(1e-4));
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
            float h = water::height_at(water, {x, 0.0f}, time);
            if (h > best_h) { best_h = h; best_x = x; }
        }
        return best_x;
    };

    REQUIRE(crest_x(0.1f) > crest_x(0.0f));
}

// --- What scripts are told about a sea ---

TEST_CASE("A sea's wave height is the sum of its trains", "[fjwater]") {
    // Scripts read this through Water::wave_height(), so what it means is part
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

    // A file the current writer produced is taken at face value.
    auto fresh_path = (dir / "new.fjwater").string();
    REQUIRE(save_fjwater_file(fresh_path, *loaded));
    auto reloaded = load_fjwater_file(fresh_path);
    REQUIRE(reloaded);
    REQUIRE(gerstner(*reloaded).steepness_headroom == Approx(gerstner(*loaded).steepness_headroom));

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
    REQUIRE(sizeof(GpuWaterField) == sizeof(FjellWaterField));

    REQUIRE(offsetof(GpuWaterWave, direction) == offsetof(FjellWaterWave, direction));
    REQUIRE(offsetof(GpuWaterWave, wavelength) == offsetof(FjellWaterWave, wavelength));
    REQUIRE(offsetof(GpuWaterWave, amplitude) == offsetof(FjellWaterWave, amplitude));
    REQUIRE(offsetof(GpuWaterWave, steepness) == offsetof(FjellWaterWave, steepness));
    REQUIRE(offsetof(GpuWaterWave, speed) == offsetof(FjellWaterWave, speed));

    REQUIRE(offsetof(GpuWaterField, waves) == offsetof(FjellWaterField, waves));
    REQUIRE(offsetof(GpuWaterField, wave_count) == offsetof(FjellWaterField, wave_count));
    REQUIRE(offsetof(GpuWaterField, amplitude_scale)
            == offsetof(FjellWaterField, amplitude_scale));
    REQUIRE(offsetof(GpuWaterField, speed_scale) == offsetof(FjellWaterField, speed_scale));
    REQUIRE(offsetof(GpuWaterField, steepness_headroom)
            == offsetof(FjellWaterField, steepness_headroom));

    // The array stride is what a matching sizeof() can still hide: two
    // structs of equal size whose arrays step differently put every wave
    // after the first in the wrong place.
    GpuWaterField cpu{};
    FjellWaterField gpu{};
    auto cpu_stride = reinterpret_cast<const char*>(&cpu.waves[1])
                    - reinterpret_cast<const char*>(&cpu.waves[0]);
    auto gpu_stride = reinterpret_cast<const char*>(&gpu.waves[1])
                    - reinterpret_cast<const char*>(&gpu.waves[0]);
    REQUIRE(cpu_stride == gpu_stride);
}

TEST_CASE("Packing for the GPU keeps the sea the CPU sees", "[fjwater]") {
    FjWaterData water = sea();
    gerstner(water).amplitude_scale = 1.6f;
    gerstner(water).speed_scale = 0.7f;
    gerstner(water).waves = {
        wave({1.0f, 0.3f}, 28.0f, 1.4f, 0.85f, 1.1f),
        wave({0.0f, 0.0f}, 20.0f, 1.0f, 0.5f),    // no direction, dropped
        wave({-0.4f, 1.0f}, 13.0f, 0.6f, 0.7f, 1.3f),
        wave({1.0f, 0.0f}, 0.0f, 1.0f, 0.5f),     // no wavelength, dropped
    };

    auto field = to_gpu_field(water);
    REQUIRE(field.wave_count == 2);
    REQUIRE(field.amplitude_scale == Approx(1.6f));
    REQUIRE(field.speed_scale == Approx(0.7f));
    REQUIRE(field.waves[0].wavelength == Approx(28.0f));
    REQUIRE(field.waves[1].wavelength == Approx(13.0f));

    // The packed field and the authored one describe the same surface: the
    // dropped trains were the ones neither evaluator would have summed.
    FjellWaterField shader_field{};
    shader_field.wave_count = field.wave_count;
    shader_field.amplitude_scale = field.amplitude_scale;
    shader_field.speed_scale = field.speed_scale;
    shader_field.steepness_headroom = field.steepness_headroom;
    for (std::uint32_t i = 0; i < field.wave_count; ++i) {
        shader_field.waves[i].direction = field.waves[i].direction;
        shader_field.waves[i].wavelength = field.waves[i].wavelength;
        shader_field.waves[i].amplitude = field.waves[i].amplitude;
        shader_field.waves[i].steepness = field.waves[i].steepness;
        shader_field.waves[i].speed = field.waves[i].speed;
    }

    for (float x = -20.0f; x <= 20.0f; x += 5.0f) {
        glm::vec2 p(x, x * 0.4f);
        INFO("at x=" << x);
        REQUIRE(water::displace(water, p, 3.0f).y
                == Approx(fjell_water_displace(shader_field, p, 3.0f).y).margin(1e-3));
    }
}

TEST_CASE("Packing drops trains past the limit", "[fjwater]") {
    FjWaterData water = sea();
    for (int i = 0; i < 40; ++i) {
        gerstner(water).waves.push_back(wave({1.0f, 0.0f}, 10.0f + static_cast<float>(i), 0.1f, 0.5f));
    }
    REQUIRE(to_gpu_field(water).wave_count == MAX_WATER_WAVES);
}
