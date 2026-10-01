#include "renderer/resources/water_refraction.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr float PI = 3.14159265358979323846f;

// A warp grid of `samples` a side over `size` metres from the origin, at
// depths `depth(x, z)` in samples. No land marks between samples.
template <typename Fn>
water::ShelterLand ground(std::uint32_t samples, float size, Fn&& depth) {
    water::ShelterLand land;
    land.size = size;
    land.warp_samples = samples;
    land.warp_depths.resize(static_cast<std::size_t>(samples) * samples);
    for (std::uint32_t z = 0; z < samples; ++z) {
        for (std::uint32_t x = 0; x < samples; ++x) {
            land.warp_depths[(static_cast<std::size_t>(z) * samples) + x] = depth(x, z);
        }
    }
    return land;
}

// k / k_deep at depth d for deep wavenumber k, from x tanh(x) = k d solved
// here by Newton.
double solved_shortening(double k, double d) {
    const double mu = k * d;
    double x = mu < 1.0 ? std::sqrt(mu) : mu;
    for (int i = 0; i < 60; ++i) {
        const double t = std::tanh(x);
        x -= ((x * t) - mu) / (t + (x * (1.0 - (t * t))));
    }
    return x / mu;
}

struct Field {
    std::vector<double> at;
    std::uint32_t samples;
    double cell;

    [[nodiscard]] double operator()(std::uint32_t x, std::uint32_t z) const {
        return at[(static_cast<std::size_t>(z) * samples) + x];
    }
    // The gradient by central differences, in arrival metres a metre.
    [[nodiscard]] double along_x(std::uint32_t x, std::uint32_t z) const {
        return ((*this)(x + 1, z) - (*this)(x - 1, z)) / (2.0 * cell);
    }
    [[nodiscard]] double along_z(std::uint32_t x, std::uint32_t z) const {
        return ((*this)(x, z + 1) - (*this)(x, z - 1)) / (2.0 * cell);
    }
};

Field arrival_over(const water::ShelterLand& land, glm::vec2 travel, float k) {
    return {water::arrival(land, {travel, k}), land.warp_samples,
            static_cast<double>(land.size) / (land.warp_samples - 1)};
}

} // namespace

TEST_CASE("shortening_at follows the dispersion relation", "[water_refraction]") {
    constexpr double K = 2.0 * PI / 40.0;
    for (const double depth : {0.05, 0.1, 0.3, 1.0, 3.0, 10.0, 30.0}) {
        const double expected = std::min(solved_shortening(K, depth),
                                         static_cast<double>(water::MAX_SHORTENING));
        CAPTURE(depth);
        CHECK(water::shortening_at(static_cast<float>(K * depth)) == Approx(expected).epsilon(1e-5));
    }
    CHECK(water::shortening_at(0.0f) == 1.0f);
    CHECK(water::shortening_at(-2.0f) == 1.0f);
    CHECK(water::shortening_at(water::SHOALING_DEEP_MU) == 1.0f);
}

TEST_CASE("Over deep water the waves arrive as the plane wave they are", "[water_refraction]") {
    const auto land = ground(65, 256.0f, [](std::uint32_t, std::uint32_t) { return 500.0f; });
    const glm::vec2 travel{0.8f, -0.6f};
    const Field a = arrival_over(land, travel, 2.0f * PI / 40.0f);
    const glm::dvec2 exact = glm::normalize(glm::dvec2(travel));
    for (std::uint32_t z = 0; z < 65; z += 4) {
        for (std::uint32_t x = 0; x < 65; x += 4) {
            const double expected = (exact.x * x * a.cell) + (exact.y * z * a.cell);
            CHECK(a(x, z) == Approx(expected).margin(1e-6));
        }
    }
}

TEST_CASE("Over a level shelf the waves arrive later by the shortening", "[water_refraction]") {
    // 3 m of water in a channel walled at its sides, a 40 m wave travelling
    // +x: every metre it gathers k(3 m)/k_deep metres of the deep sea's phase.
    constexpr float K = 2.0f * PI / 40.0f;
    const auto land = ground(101, 400.0f, [](std::uint32_t, std::uint32_t z) {
        return z == 0 || z == 100 ? -1.0f : 3.0f;
    });
    const Field a = arrival_over(land, {1.0f, 0.0f}, K);
    const double per_metre = solved_shortening(K, 3.0);
    for (const std::uint32_t z : {1u, 50u, 99u}) {
        for (const std::uint32_t x : {10u, 50u, 100u}) {
            CHECK(a(x, z) == Approx(per_metre * x * a.cell).epsilon(1e-5));
        }
    }
}

TEST_CASE("Up a beach met at an angle the crests turn by Snell's law", "[water_refraction]") {
    // Deep water to x = 150 m, then a beach rising toward +x to 0.5 m deep at
    // 400 m; a 40 m wave arriving 30 degrees off the beach's normal. The bed
    // is the same all along z, so the wave keeps its wavenumber along z, k_deep
    // sin 30, and takes what is left of k(d) across it. Land along the bottom
    // edge keeps the sea from also coming in over the shallows there.
    constexpr float K = 2.0f * PI / 40.0f;
    const auto depth_at = [](double x) {
        return x < 150.0 ? 60.0 : 60.0 - ((x - 150.0) * (59.5 / 250.0));
    };
    const auto land = ground(201, 400.0f, [&](std::uint32_t x, std::uint32_t z) {
        return z == 0 ? -1.0f : static_cast<float>(depth_at(2.0 * x));
    });
    const glm::vec2 travel{std::cos(PI / 6.0f), std::sin(PI / 6.0f)};
    const Field a = arrival_over(land, travel, K);

    for (const std::uint32_t x : {50u, 100u, 150u, 180u, 195u}) {
        const std::uint32_t z = 180;
        const double kappa = solved_shortening(K, depth_at(2.0 * x));
        CAPTURE(x, kappa);
        CHECK(a.along_z(x, z) == Approx(0.5).epsilon(0.01));
        CHECK(a.along_x(x, z) == Approx(std::sqrt((kappa * kappa) - 0.25)).epsilon(0.01));
    }
}

namespace {

// Deep water 300 m across, a wave travelling +x, and a wall of land samples
// across its way at x = 100 m from z = 100 to 200 m, on cells of `cell`
// metres. Behind the wall a point is reached from the nearer end, as the deep
// sea reached that end plus the straight way from there, and the crests there
// are rings round the end. How far the solve is from that, at points around
// the shadow: the worst arrival, in metres, and the worst way the crests
// face, in degrees, more than a wavelength from the end.
struct WallError {
    double arrival{0.0};
    double facing{0.0};
};

WallError behind_a_wall(double cell) {
    const auto samples = static_cast<std::uint32_t>(std::lround(300.0 / cell)) + 1;
    const auto land = ground(samples, 300.0f, [&](std::uint32_t x, std::uint32_t z) {
        const double px = x * cell;
        const double pz = z * cell;
        return std::abs(px - 100.0) < 0.5 * cell && pz >= 100.0 && pz <= 200.0 ? -1.0f : 500.0f;
    });
    const Field a = arrival_over(land, {1.0f, 0.0f}, 2.0f * PI / 40.0f);

    // A front passes an end between its last land sample and the water sample
    // next to it.
    const glm::dvec2 low{100.0, 100.0 - (0.5 * cell)};
    const glm::dvec2 high{100.0, 200.0 + (0.5 * cell)};
    WallError worst;
    for (const double px : {120.0, 160.0, 240.0, 298.0}) {
        for (const double pz : {110.0, 120.0, 130.0, 170.0, 180.0, 190.0}) {
            const auto x = static_cast<std::uint32_t>(std::lround(px / cell));
            const auto z = static_cast<std::uint32_t>(std::lround(pz / cell));
            const glm::dvec2 p{x * cell, z * cell};
            const glm::dvec2 end = glm::length(p - low) < glm::length(p - high) ? low : high;
            worst.arrival =
                std::max(worst.arrival, std::abs(a(x, z) - (100.0 + glm::length(p - end))));
            if (glm::length(p - end) < 40.0) continue;
            const glm::dvec2 slope{a.along_x(x, z), a.along_z(x, z)};
            const double off = std::acos(std::clamp(
                glm::dot(glm::normalize(slope), glm::normalize(p - end)), -1.0, 1.0));
            worst.facing = std::max(worst.facing, glm::degrees(off));
        }
    }
    return worst;
}

} // namespace

TEST_CASE("Behind a wall the waves arrive by the way round its ends", "[water_refraction]") {
    // The solve is first order, its error gathering where a front is most
    // curved: round the end, and along the edge of the shadow. On the warp's
    // own 2 m it is within a cell and a few degrees; what shows it is right is
    // that the error halves, near enough, with the cell.
    const WallError coarse = behind_a_wall(2.0);
    const WallError fine = behind_a_wall(1.0);
    CHECK(coarse.arrival < 2.0);
    CHECK(coarse.facing < 5.0);
    CHECK(fine.arrival < 0.65 * coarse.arrival);
    CHECK(fine.facing < 0.65 * coarse.facing);
}

TEST_CASE("Out of a wall's shadow the waves arrive as they were", "[water_refraction]") {
    const auto land = ground(151, 300.0f, [](std::uint32_t x, std::uint32_t z) {
        return x == 50 && z >= 50 && z <= 100 ? -1.0f : 500.0f;
    });
    const Field a = arrival_over(land, {1.0f, 0.0f}, 2.0f * PI / 40.0f);
    CHECK(a(120, 20) == Approx(240.0).margin(1e-6));
    CHECK(a(40, 75) == Approx(80.0).margin(1e-6));
}

TEST_CASE("Land between two samples stops a front", "[water_refraction]") {
    // A wall marked only on the land's finer grid, between two warp samples:
    // the water behind it is reached the way round, not through it.
    auto land = ground(65, 128.0f, [](std::uint32_t, std::uint32_t) { return 500.0f; });
    land.samples = 129;
    land.land.assign(129u * 129u, 0);
    for (std::uint32_t z = 32; z <= 96; ++z) land.land[(z * 129u) + 65u] = 1;   // x = 65 m
    const Field a = arrival_over(land, {1.0f, 0.0f}, 2.0f * PI / 40.0f);
    // Just behind the wall's middle, at x = 66 m: well past the 66 m the plane
    // wave would have come.
    CHECK(a(33, 32) > 66.0 + 20.0);
    // In front of it, as it was.
    CHECK(a(32, 32) == Approx(64.0).margin(1e-6));
}

TEST_CASE("Water no front reaches is unreached, and so is land", "[water_refraction]") {
    // A pond walled in by land in the middle of the sea.
    const auto land = ground(41, 80.0f, [](std::uint32_t x, std::uint32_t z) {
        const bool ring = (x == 15 || x == 25) ? (z >= 15 && z <= 25)
                        : (z == 15 || z == 25) ? (x >= 15 && x <= 25)
                                               : false;
        return ring ? -1.0f : 10.0f;
    });
    const Field a = arrival_over(land, {1.0f, 0.0f}, 2.0f * PI / 40.0f);
    CHECK(std::isinf(a(20, 20)));
    CHECK(std::isinf(a(15, 20)));
    CHECK(std::isfinite(a(30, 20)));
}
