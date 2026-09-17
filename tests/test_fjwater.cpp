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
#include <vector>

using namespace fjell;
using Catch::Approx;

namespace {

/// The same field, in the shape each evaluator wants.
struct FieldPair {
    FjWaterData cpu;
    FjellWaterField gpu{};
};

[[nodiscard]] FieldPair make_field(const std::vector<WaterWave>& waves,
                                   float amplitude_scale = 1.0f,
                                   float speed_scale = 1.0f) {
    FieldPair pair;
    pair.cpu.waves = waves;
    pair.cpu.amplitude_scale = amplitude_scale;
    pair.cpu.speed_scale = speed_scale;

    pair.gpu.wave_count = static_cast<unsigned int>(waves.size());
    pair.gpu.amplitude_scale = amplitude_scale;
    pair.gpu.speed_scale = speed_scale;
    pair.gpu.steepness_headroom = pair.cpu.steepness_headroom;
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

// --- The field behaves like water ---

TEST_CASE("Flat water is flat", "[fjwater]") {
    FjWaterData water;  // no waves
    REQUIRE(water::height_at(water, {3.0f, 7.0f}, 5.0f) == Approx(0.0f));
    REQUIRE(water::normal_at(water, {3.0f, 7.0f}, 5.0f).y == Approx(1.0f));
    REQUIRE(water::crest(water, {3.0f, 7.0f}, 5.0f) == Approx(0.0f));
}

TEST_CASE("A wave stays within its amplitude", "[fjwater]") {
    FjWaterData water;
    water.waves = {wave({1.0f, 0.0f}, 20.0f, 1.5f, 0.9f)};

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
        FjWaterData water;
        water.waves = {wave({1.0f, 0.0f}, 20.0f, 1.0f, steepness)};
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
    FjWaterData water;
    water.waves = {wave({1.0f, 0.0f}, 10.0f, 2.0f, 1.0f)};

    for (float x = 0.0f; x < 20.0f; x += 0.01f) {
        REQUIRE(water::crest(water, {x, 0.0f}, 0.0f) <= 1.0f);
    }
}

TEST_CASE("The iterative height lands on the surface", "[fjwater]") {
    // height_at() reads the field at a rest position; under steep waves the
    // point actually above a world XZ started somewhere else. The iterative
    // form walks that back, so displacing from its answer should return to
    // the column it was asked about.
    FjWaterData water;
    water.waves = {
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
    FjWaterData water;
    water.waves = {wave({1.0f, 0.0f}, 20.0f, 1.0f, 0.0f)};  // pure sine, no pinch

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

// --- Generator ---

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

    FjWaterData written;
    written.waves = {
        wave({1.0f, 0.3f}, 28.0f, 1.4f, 0.85f, 1.1f),
        wave({-0.4f, 1.0f}, 13.0f, 0.6f, 0.7f, 1.3f),
    };
    written.amplitude_scale = 1.7f;
    written.speed_scale = 0.8f;
    written.generator.wind_direction = {0.6f, 0.8f};
    written.generator.swell = 2.2f;
    written.generator.wave_count = 5;

    REQUIRE(save_fjwater_file(path, written));

    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(loaded->waves.size() == 2);
    REQUIRE(loaded->amplitude_scale == Approx(1.7f));
    REQUIRE(loaded->speed_scale == Approx(0.8f));
    REQUIRE(loaded->waves[0].wavelength == Approx(28.0f));
    REQUIRE(loaded->waves[0].steepness == Approx(0.85f));
    REQUIRE(loaded->waves[1].speed == Approx(1.3f));
    // The generator settings come back so the editor can generate again.
    REQUIRE(loaded->generator.swell == Approx(2.2f));
    REQUIRE(loaded->generator.wave_count == 5);

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
    REQUIRE(loaded->waves.size() == 1);
    REQUIRE(loaded->waves[0].wavelength == Approx(12.0f));

    std::filesystem::remove_all(dir);
}

TEST_CASE("A .fjwater keeps at most MAX_WATER_WAVES trains", "[fjwater]") {
    auto dir = std::filesystem::temp_directory_path() / "fjell-fjwater-many";
    std::filesystem::create_directories(dir);
    auto path = (dir / "many.fjwater").string();

    FjWaterData written;
    for (int i = 0; i < 40; ++i) {
        written.waves.push_back(wave({1.0f, 0.0f}, 10.0f + static_cast<float>(i), 0.1f, 0.5f));
    }
    REQUIRE(save_fjwater_file(path, written));

    auto loaded = load_fjwater_file(path);
    REQUIRE(loaded);
    REQUIRE(loaded->waves.size() == MAX_WATER_WAVES);

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
    FjWaterData water;
    water.amplitude_scale = 1.6f;
    water.speed_scale = 0.7f;
    water.waves = {
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
    FjWaterData water;
    for (int i = 0; i < 40; ++i) {
        water.waves.push_back(wave({1.0f, 0.0f}, 10.0f + static_cast<float>(i), 0.1f, 0.5f));
    }
    REQUIRE(to_gpu_field(water).wave_count == MAX_WATER_WAVES);
}
