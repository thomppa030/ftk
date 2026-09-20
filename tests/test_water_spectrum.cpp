#include "renderer/resources/water_spectrum.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <vector>

using namespace fjell;
using Catch::Approx;

namespace {

constexpr float GRAVITY = 9.81f;
constexpr float PI = 3.14159265358979323846f;
constexpr double PI_D = 3.14159265358979323846;

/// Variance of the surface height the spectrum describes: its integral over
/// frequency, by the midpoint rule on a grid far finer than the peak is wide.
[[nodiscard]] double height_variance(const FftContributor& sea, float from = 0.01f,
                                     float to = 40.0f) {
    const int steps = 400000;
    const double width = (static_cast<double>(to) - from) / steps;
    double sum = 0.0;
    for (int i = 0; i < steps; ++i) {
        const auto omega = static_cast<float>(from + (i + 0.5) * width);
        sum += static_cast<double>(water::jonswap(sea, omega)) * width;
    }
    return sum;
}

/// What the grid holds if nothing were random: the spectrum summed over it.
[[nodiscard]] double grid_variance(const FftContributor& sea) {
    const std::uint32_t n = sea.resolution;
    const float dk = 2.0f * PI / sea.patch_size;
    double sum = 0.0;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const auto kx = static_cast<float>(x < n / 2 ? static_cast<int>(x)
                                                         : static_cast<int>(x) - static_cast<int>(n));
            const auto kz = static_cast<float>(z < n / 2 ? static_cast<int>(z)
                                                         : static_cast<int>(z) - static_cast<int>(n));
            sum += static_cast<double>(water::wavenumber_spectrum(sea, {kx * dk, kz * dk})) * dk * dk;
        }
    }
    return sum;
}

} // namespace

TEST_CASE("The spectrum peaks at its peak frequency", "[water_spectrum]") {
    // Not a convention of this code: the JONSWAP shape is built so that its
    // maximum falls exactly on the peak frequency, whatever the wind.
    for (float wind : {4.0f, 10.0f, 25.0f}) {
        FftContributor sea;
        sea.wind_speed = wind;
        const float peak = water::peak_frequency(sea);

        float best_omega = 0.0f;
        float best = 0.0f;
        for (float omega = 0.05f; omega < 12.0f; omega += 0.0005f) {
            const float density = water::jonswap(sea, omega);
            if (density > best) { best = density; best_omega = omega; }
        }
        INFO("wind " << wind);
        REQUIRE(best_omega == Approx(peak).epsilon(0.005));
    }
}

TEST_CASE("The spectrum holds the energy JONSWAP measured", "[water_spectrum]") {
    FftContributor sea;
    sea.wind_speed = 10.0f;
    sea.fetch = 100000.0f;

    // A JONSWAP spectrum with the measured peak enhancement of 3.3 carries
    // 1.52 times the energy of the Pierson-Moskowitz spectrum under it, which
    // integrates in closed form to alpha g^2 / (5 omega_p^4). Both figures are
    // the published ones, so this pins the shape and its constants from
    // outside this code.
    const double peak = water::peak_frequency(sea);
    const double alpha = 0.076 * std::pow(10.0 * 10.0 / (100000.0 * GRAVITY), 0.22);
    const double published = 1.52 * alpha * GRAVITY * GRAVITY / (5.0 * std::pow(peak, 4.0));
    const double variance = height_variance(sea);
    REQUIRE(variance == Approx(published).epsilon(0.03));

    // And against the sea itself rather than the formula: the JONSWAP
    // measurements put the significant wave height of a fetch-limited sea at
    // 4 sqrt(1.6e-7 x) U^2 / g for a dimensionless fetch x = g F / U^2 — about
    // 1.6 m here. The parametric spectrum is known to run above its own
    // growth law at long fetch, so this is a band rather than a match; what it
    // catches is a wrong unit or a stray 2 pi, which miss by far more.
    const double x = GRAVITY * 100000.0 / (10.0 * 10.0);
    const double measured_hs = 4.0 * std::sqrt(1.6e-7 * x) * 10.0 * 10.0 / GRAVITY;
    const double hs = 4.0 * std::sqrt(variance);
    REQUIRE(hs > measured_hs * 0.8);
    REQUIRE(hs < measured_hs * 1.5);
}

TEST_CASE("More wind and more fetch make a longer, taller sea", "[water_spectrum]") {
    FftContributor lake;
    lake.wind_speed = 8.0f;
    lake.fetch = 5000.0f;
    FftContributor ocean = lake;
    ocean.fetch = 300000.0f;
    FftContributor gale = lake;
    gale.wind_speed = 20.0f;

    REQUIRE(water::peak_frequency(ocean) < water::peak_frequency(lake));
    REQUIRE(water::peak_frequency(gale) < water::peak_frequency(lake));
    REQUIRE(height_variance(ocean) > height_variance(lake));
    REQUIRE(height_variance(gale) > height_variance(lake));

    FftContributor calm = lake;
    calm.wind_speed = 0.0f;
    REQUIRE(water::jonswap(calm, 1.0f) == 0.0f);
    REQUIRE(water::peak_frequency(calm) == 0.0f);
}

TEST_CASE("Spreading shares energy out without adding any", "[water_spectrum]") {
    for (float spread : {5.0f, 35.0f, 80.0f}) {
        FftContributor sea;
        sea.spread_degrees = spread;

        const int steps = 20000;
        double total = 0.0;
        for (int i = 0; i < steps; ++i) {
            const auto angle = static_cast<float>(-PI + (i + 0.5) * 2.0 * PI / steps);
            total += static_cast<double>(water::directional_spread(sea, angle)) * 2.0 * PI / steps;
        }
        INFO("spread " << spread);
        REQUIRE(total == Approx(1.0).epsilon(0.002));

        // The dial means what it says: half the downwind density at that angle.
        REQUIRE(water::directional_spread(sea, glm::radians(spread))
                == Approx(0.5f * water::directional_spread(sea, 0.0f)).epsilon(0.001));
        // And nothing travels straight upwind.
        REQUIRE(water::directional_spread(sea, PI) == Approx(0.0f).margin(1e-6));
    }
}

TEST_CASE("The grid holds the variance the spectrum has", "[water_spectrum]") {
    // The change of variables from frequency to the wavevector plane is where
    // a factor goes missing if one is going to: the sum over the grid has to
    // come back to the integral over frequency it was derived from. Compared
    // over the band of frequencies the grid can hold.
    FftContributor sea;
    sea.wind_speed = 10.0f;
    sea.wind_direction = {0.6f, -0.8f};
    sea.patch_size = 600.0f;
    sea.resolution = 256;

    const float dk = 2.0f * PI / sea.patch_size;
    const float lowest = std::sqrt(GRAVITY * dk * 0.5f);
    const float highest = std::sqrt(GRAVITY * dk * static_cast<float>(sea.resolution / 2));
    const double expected = height_variance(sea, lowest, highest);
    REQUIRE(grid_variance(sea) == Approx(expected).epsilon(0.08));

    // Height scales the surface, so it scales the variance by its square.
    FftContributor doubled = sea;
    doubled.amplitude_scale = 2.0f;
    REQUIRE(grid_variance(doubled) == Approx(4.0 * grid_variance(sea)).epsilon(1e-4));
}

TEST_CASE("A drawn sea animates into a real surface of the right height", "[water_spectrum]") {
    // The claim generate_spectrum() makes: pair every wavevector with the
    // conjugate of its opposite, transform, and what comes out is a real
    // heightfield whose variance is the spectrum's. Done here with a plain
    // Fourier sum, at a size where that is affordable.
    FftContributor sea;
    sea.wind_speed = 10.0f;
    sea.patch_size = 300.0f;
    sea.resolution = 32;
    const std::uint32_t n = sea.resolution;
    const double expected = grid_variance(sea);

    double mean_variance = 0.0;
    double worst_imaginary = 0.0;
    const int seas = 96;
    for (int s = 0; s < seas; ++s) {
        sea.seed = static_cast<std::uint32_t>(s + 1);
        const water::SpectrumField field = water::generate_spectrum(sea);
        REQUIRE(field.resolution == n);

        std::vector<std::complex<double>> spectrum(n * n);
        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                const glm::vec2 here = field.amplitudes[z * n + x];
                const glm::vec2 opposite = field.amplitudes[((n - z) % n) * n + (n - x) % n];
                spectrum[z * n + x] = {here.x + opposite.x, here.y - opposite.y};
            }
        }

        // Inverse transform, one axis at a time.
        std::vector<std::complex<double>> rows(n * n);
        std::vector<std::complex<double>> heights(n * n);
        for (std::uint32_t z = 0; z < n; ++z)
            for (std::uint32_t x = 0; x < n; ++x)
                for (std::uint32_t k = 0; k < n; ++k)
                    rows[z * n + x] += spectrum[z * n + k]
                                     * std::polar(1.0, 2.0 * PI_D * k * x / n);
        for (std::uint32_t z = 0; z < n; ++z)
            for (std::uint32_t x = 0; x < n; ++x)
                for (std::uint32_t k = 0; k < n; ++k)
                    heights[z * n + x] += rows[k * n + x]
                                        * std::polar(1.0, 2.0 * PI_D * k * z / n);

        double variance = 0.0;
        for (const auto& h : heights) {
            variance += h.real() * h.real();
            worst_imaginary = std::max(worst_imaginary, std::abs(h.imag()));
        }
        mean_variance += variance / (n * n) / seas;
    }

    REQUIRE(worst_imaginary < 1e-6);
    // Averaged over many seas, since any one of them is a draw.
    REQUIRE(mean_variance == Approx(expected).epsilon(0.1));
}

TEST_CASE("The sea runs downwind", "[water_spectrum]") {
    FftContributor sea;
    sea.wind_direction = {0.0f, 1.0f};
    const glm::vec2 downwind{0.0f, 0.1f};
    REQUIRE(water::wavenumber_spectrum(sea, downwind) > 0.0f);
    REQUIRE(water::wavenumber_spectrum(sea, -downwind) == Approx(0.0f).margin(1e-9));
    REQUIRE(water::wavenumber_spectrum(sea, {0.1f, 0.0f})
            < water::wavenumber_spectrum(sea, downwind));
    REQUIRE(water::wavenumber_spectrum(sea, {0.0f, 0.0f}) == 0.0f);
}

TEST_CASE("The high cut takes out the short waves and leaves the long", "[water_spectrum]") {
    FftContributor sea;
    FftContributor cut = sea;
    cut.high_cut = 8.0f;

    const glm::vec2 swell{2.0f * PI / 80.0f, 0.0f};
    const glm::vec2 ripple{2.0f * PI / 1.0f, 0.0f};
    REQUIRE(water::wavenumber_spectrum(cut, swell)
            == Approx(water::wavenumber_spectrum(sea, swell)).epsilon(0.02));
    REQUIRE(water::wavenumber_spectrum(cut, ripple)
            < 1e-6f * water::wavenumber_spectrum(sea, ripple));
}

TEST_CASE("A seed is one sea, and another seed another", "[water_spectrum]") {
    FftContributor sea;
    sea.resolution = 64;
    const auto a = water::generate_spectrum(sea);
    const auto b = water::generate_spectrum(sea);
    REQUIRE(a.amplitudes == b.amplitudes);

    sea.seed = 2;
    const auto c = water::generate_spectrum(sea);
    REQUIRE(a.amplitudes != c.amplitudes);

    // The mean of the surface is the node's height, not the spectrum's.
    REQUIRE(a.amplitudes[0] == glm::vec2(0.0f));
}

TEST_CASE("A spectrum cannot be drawn on no grid", "[water_spectrum]") {
    FftContributor sea;
    sea.resolution = 1;
    REQUIRE(water::generate_spectrum(sea).amplitudes.empty());

    sea.resolution = 100;   // rounded down to a power of two
    REQUIRE(water::generate_spectrum(sea).resolution == 64);

    sea.patch_size = 0.0f;
    REQUIRE(water::generate_spectrum(sea).amplitudes.empty());
}
