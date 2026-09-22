#include "renderer/systems/particle_budget.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace fjell;

namespace {
constexpr uint32_t PARTICLES = MAX_SCENE_PARTICLES;
constexpr uint32_t EMITTERS = MAX_SCENE_EMITTERS;
} // namespace

TEST_CASE("A mega-buffer doubles while there is room", "[particles]") {
    // Growth is amortised so a scene adding emitters one at a time does not
    // rebind pages every time.
    REQUIRE(grow_buffer_to(513, 512, PARTICLES) == 1024);
    REQUIRE(grow_buffer_to(4097, 4096, PARTICLES) == 8192);
    REQUIRE(grow_buffer_to(32769, 32768, PARTICLES) == 65536);
}

TEST_CASE("Growth never asks for more than the buffer can hold", "[particles]") {
    // The buffers are sparse with a fixed virtual size, and asking past it
    // throws — fatally, where the request that prompted it was not. Doubling
    // is a heuristic and has to yield to the ceiling.
    REQUIRE(grow_buffer_to(PARTICLES, PARTICLES, PARTICLES) == PARTICLES);
    REQUIRE(grow_buffer_to(EMITTERS, EMITTERS, EMITTERS) == EMITTERS);
}

TEST_CASE("A full scene survives one more emitter", "[particles]") {
    // The case that took the engine down with rain and a sea in one scene.
    // Particles and emitters share a growth branch, so an emitter count that
    // trips the rebuild doubles the particle request too — even when the
    // particles have all the room they need. At the particle ceiling that
    // asked for twice it and threw.
    REQUIRE(grow_buffer_to(30000, PARTICLES, PARTICLES) == PARTICLES);

    // And the emitter side of the same branch.
    REQUIRE(grow_buffer_to(9, EMITTERS, EMITTERS) == EMITTERS);
}

TEST_CASE("Growth from nothing asks for what was wanted", "[particles]") {
    // Doubling zero is zero; a buffer that has never grown still has to
    // arrive at a usable size.
    REQUIRE(grow_buffer_to(1000, 0, PARTICLES) == 1000);
    REQUIRE(grow_buffer_to(0, 0, PARTICLES) == 0);
}

TEST_CASE("A request at the ceiling is met exactly", "[particles]") {
    // The scene budget refuses emitters past MAX_SCENE_PARTICLES, so a request
    // can legitimately land on the ceiling and must be honoured rather than
    // rounded down by the clamp.
    REQUIRE(grow_buffer_to(PARTICLES, 1024, PARTICLES) == PARTICLES);
}

TEST_CASE("A new emitter takes the first gap that holds it", "[particles]") {
    // Rain that stops and starts while a pooled one-shot sits above it must
    // land back where it was, not climb past the one-shot every time.
    std::vector<ParticleRange> used{{11600, 21600}};
    REQUIRE(first_fit_range(used, 11600, PARTICLES) == 0);
    REQUIRE(first_fit_range(used, 11601, PARTICLES) == 21600);

    // A gap between two live emitters is found regardless of slot order.
    used = {{30000, 31000}, {0, 1000}, {5000, 6000}};
    REQUIRE(first_fit_range(used, 4000, PARTICLES) == 1000);
    REQUIRE(first_fit_range(used, 4001, PARTICLES) == 6000);
}

TEST_CASE("Nothing fits past the ceiling", "[particles]") {
    std::vector<ParticleRange> used{{0, PARTICLES - 100}};
    REQUIRE(first_fit_range(used, 100, PARTICLES) == PARTICLES - 100);
    REQUIRE_FALSE(first_fit_range(used, 101, PARTICLES).has_value());
    REQUIRE_FALSE(first_fit_range({}, PARTICLES + 1, PARTICLES).has_value());
}

TEST_CASE("An empty buffer starts at zero", "[particles]") {
    REQUIRE(first_fit_range({}, 500, PARTICLES) == 0);
    REQUIRE(first_fit_range({}, PARTICLES, PARTICLES) == 0);
}
