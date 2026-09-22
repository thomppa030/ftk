#include "core/simd_trig.hpp"
#include "renderer/resources/water_spectrum.hpp"
#include "renderer/resources/water_surface.hpp"

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

/// The sea as physics describes it, with no stylisation on top: the case the
/// published figures and the arithmetic below are about. Spelled out rather
/// than left to the contributor's defaults, which are a stylised sea.
[[nodiscard]] FftContributor physical_sea() {
    FftContributor sea;
    sea.wind_direction = {1.0f, 0.0f};
    sea.wind_speed = 8.0f;
    sea.fetch = 100000.0f;
    sea.spread_degrees = 35.0f;
    sea.high_cut = 0.0f;
    sea.choppiness = 1.0f;
    sea.amplitude_scale = 1.0f;
    return sea;
}

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
        FftContributor sea = physical_sea();
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
    FftContributor sea = physical_sea();
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
    FftContributor lake = physical_sea();
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
        FftContributor sea = physical_sea();
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
    FftContributor sea = physical_sea();
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
    FftContributor sea = physical_sea();
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
    FftContributor sea = physical_sea();
    sea.wind_direction = {0.0f, 1.0f};
    const glm::vec2 downwind{0.0f, 0.1f};
    REQUIRE(water::wavenumber_spectrum(sea, downwind) > 0.0f);
    REQUIRE(water::wavenumber_spectrum(sea, -downwind) == Approx(0.0f).margin(1e-9));
    REQUIRE(water::wavenumber_spectrum(sea, {0.1f, 0.0f})
            < water::wavenumber_spectrum(sea, downwind));
    REQUIRE(water::wavenumber_spectrum(sea, {0.0f, 0.0f}) == 0.0f);
}

TEST_CASE("The high cut takes out the short waves and leaves the long", "[water_spectrum]") {
    FftContributor sea = physical_sea();
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
    FftContributor sea = physical_sea();
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
    FftContributor sea = physical_sea();
    sea.resolution = 1;
    REQUIRE(water::generate_spectrum(sea).amplitudes.empty());

    sea.resolution = 100;   // rounded down to a power of two
    REQUIRE(water::generate_spectrum(sea).resolution == 64);

    sea.patch_size = 0.0f;
    REQUIRE(water::generate_spectrum(sea).amplitudes.empty());
}

TEST_CASE("One wavevector of a simulated sea is one Gerstner train", "[water_spectrum]") {
    // The simulated sea and the authored one are the same physics, so a
    // spectrum holding a single wave has to be the surface the Gerstner
    // evaluator gives for that wave: as tall, pinched toward its crest the
    // same way, travelling the way its wavevector points at the speed deep
    // water gives it. The Gerstner evaluator is the independent answer here —
    // it shares no code with the spectrum — and it is what buoyancy already
    // trusts.
    const std::uint32_t n = 16;
    const float patch = 48.0f;
    const float amplitude = 0.6f;

    water::SpectrumField field;
    field.resolution = n;
    field.k_spacing = 2.0f * PI / patch;
    field.amplitudes.assign(n * n, glm::vec2(0.0f));
    // Wavevector (3, 2) grid steps. A sine of this amplitude is the real part
    // of -i A e^{i theta}, and pairing with the conjugate doubles what is
    // stored, so the stored amplitude is half of -i A.
    const std::uint32_t kx = 3;
    const std::uint32_t kz = 2;
    field.amplitudes[kz * n + kx] = {0.0f, -amplitude * 0.5f};

    const glm::vec2 k{field.wavenumber(kx), field.wavenumber(kz)};
    const float k_len = glm::length(k);

    GerstnerContributor train;
    WaterWave wave;
    wave.direction = k;
    wave.wavelength = 2.0f * PI / k_len;
    wave.amplitude = amplitude;
    wave.speed = 1.0f;
    // Steepness is a share of the fold limit 1/(kA); choppiness 1 is a
    // horizontal motion as large as the vertical, which is kA of that limit.
    wave.steepness = k_len * amplitude;
    train.steepness_headroom = 1.0f;
    train.waves = {wave};
    FjWaterData gerstner;
    gerstner.contributors.emplace_back(train);
    REQUIRE(wave.steepness < 1.0f);

    for (float time : {0.0f, 0.9f, 7.3f}) {
        const auto spectra = water::evolve_spectrum(field, 1.0f, time);
        const auto height = water::inverse_transform_reference(spectra.height, n);
        const auto along_x = water::inverse_transform_reference(spectra.along_x, n);
        const auto along_z = water::inverse_transform_reference(spectra.along_z, n);

        for (std::uint32_t z = 0; z < n; z += 3) {
            for (std::uint32_t x = 0; x < n; x += 3) {
                const glm::vec2 p{static_cast<float>(x) * patch / n,
                                  static_cast<float>(z) * patch / n};
                const glm::vec3 expected = water::displace(water::apply_wind(gerstner, {0.0f, 0.0f}), p, time);
                INFO("at (" << x << ", " << z << ") t=" << time);
                REQUIRE(height[z * n + x] == Approx(expected.y).margin(1e-4));
                REQUIRE(along_x[z * n + x] == Approx(expected.x).margin(1e-4));
                REQUIRE(along_z[z * n + x] == Approx(expected.z).margin(1e-4));
            }
        }
    }
}

TEST_CASE("The sea's size in closed form is the spectrum's own", "[water_spectrum]") {
    // significant_amplitude() is a shortcut: half the significant wave height
    // from a closed form rather than from integrating the spectrum. Half the
    // significant height is two standard deviations of the surface, and the
    // variance is the spectrum's integral, which can simply be done.
    for (float wind : {5.0f, 12.0f, 24.0f}) {
        for (float fetch : {8000.0f, 150000.0f}) {
            FftContributor sea = physical_sea();
            sea.wind_speed = wind;
            sea.fetch = fetch;
            sea.amplitude_scale = 1.5f;
            INFO("wind " << wind << " fetch " << fetch);
            REQUIRE(water::significant_amplitude(sea)
                    == Approx(2.0 * std::sqrt(height_variance(sea)) * 1.5).epsilon(0.03));
        }
    }
    FftContributor calm = physical_sea();
    calm.wind_speed = 0.0f;
    REQUIRE(water::significant_amplitude(calm) == 0.0f);
}

TEST_CASE("Kept noise under a new wind is the same sea, grown", "[water_spectrum]") {
    // What lets a sea follow the weather without being rolled afresh each
    // time: the random part is fixed by the seed, so every wavevector keeps
    // its phase while the spectrum over it changes. If the phases moved, the
    // water would jump every time the wind did.
    FftContributor breeze = physical_sea();
    breeze.resolution = 64;
    breeze.wind_speed = 7.0f;
    FftContributor gale = breeze;
    gale.wind_speed = 18.0f;

    const auto noise = water::draw_noise(breeze.seed, breeze.resolution);
    const auto calm_sea = water::generate_spectrum(breeze, noise);
    const auto rough_sea = water::generate_spectrum(gale, noise);

    // Shaping from kept noise is the same draw as shaping from scratch.
    REQUIRE(calm_sea.amplitudes == water::generate_spectrum(breeze).amplitudes);

    std::size_t compared = 0;
    for (std::size_t i = 0; i < calm_sea.amplitudes.size(); ++i) {
        const glm::vec2 a = calm_sea.amplitudes[i];
        const glm::vec2 b = rough_sea.amplitudes[i];
        if (glm::length(a) < 1e-9f || glm::length(b) < 1e-9f) continue;
        // Same direction in the complex plane: same phase.
        const float cross = a.x * b.y - a.y * b.x;
        REQUIRE(std::abs(cross) <= 1e-4f * glm::length(a) * glm::length(b));
        REQUIRE(glm::dot(a, b) > 0.0f);
        ++compared;
    }
    REQUIRE(compared > 100);

    // Noise for another sea is refused rather than shaped into the wrong one.
    REQUIRE(water::generate_spectrum(breeze, water::draw_noise(breeze.seed + 1, 64))
                .amplitudes.empty());
    REQUIRE(water::generate_spectrum(breeze, water::draw_noise(breeze.seed, 32))
                .amplitudes.empty());
}

TEST_CASE("The fast transform is the plain sum", "[water_spectrum]") {
    // Two implementations of one transform: the plain Fourier sum, which has
    // no cleverness in it to be wrong, and the radix-2 one that is fast enough
    // to use. Held together at a size the plain sum can afford.
    FftContributor sea = physical_sea();
    sea.resolution = 32;
    sea.patch_size = 120.0f;
    const auto field = water::generate_spectrum(sea);
    const auto spectra = water::evolve_spectrum(field, 1.7f, 4.2f);

    for (const auto* spectrum : {&spectra.height, &spectra.along_x, &spectra.along_z}) {
        const auto plain = water::inverse_transform_reference(*spectrum, 32);
        const auto fast = water::inverse_transform(*spectrum, 32);
        REQUIRE(fast.size() == plain.size());
        for (std::size_t i = 0; i < plain.size(); ++i) {
            REQUIRE(fast[i] == Approx(plain[i]).margin(1e-4));
        }
    }

    // Not a power of two: refused, not transformed wrongly.
    const auto refused = water::inverse_transform(std::vector<glm::vec2>(36, {1.0f, 0.0f}), 6);
    for (float v : refused) REQUIRE(v == 0.0f);
}

TEST_CASE("One spectral wave folds exactly where its Gerstner train does", "[water_spectrum]") {
    // The simulated sea's Jacobian entries, against the Gerstner evaluator's
    // determinant for the same single wave — which shares no code with them
    // and is what the editor has always measured folding with. Pushed past
    // the fold limit on purpose, so the sign of the determinant is in play
    // and not only its size.
    const std::uint32_t n = 32;
    const float patch = 64.0f;
    const float amplitude = 0.9f;
    const float choppiness = 2.5f;

    water::SpectrumField field;
    field.resolution = n;
    field.k_spacing = 2.0f * PI / patch;
    field.amplitudes.assign(n * n, glm::vec2(0.0f));
    const std::uint32_t kx = 4;
    const std::uint32_t kz = 3;
    field.amplitudes[kz * n + kx] = {0.0f, -amplitude * 0.5f};   // a sine of this amplitude

    const glm::vec2 k{field.wavenumber(kx), field.wavenumber(kz)};
    const float k_len = glm::length(k);

    // The same wave as a Gerstner train. Its horizontal motion is q A with
    // q = steepness * headroom / (k A); choppiness c asks for c A, so with the
    // steepness at its ceiling of 1 the headroom is c k A.
    GerstnerContributor train;
    WaterWave wave;
    wave.direction = k;
    wave.wavelength = 2.0f * PI / k_len;
    wave.amplitude = amplitude;
    wave.steepness = 1.0f;
    train.steepness_headroom = choppiness * k_len * amplitude;
    train.waves = {wave};
    FjWaterData gerstner;
    gerstner.contributors.emplace_back(train);
    REQUIRE(train.steepness_headroom > 1.0f);   // past the fold limit: it must fold somewhere

    const float time = 3.1f;
    const auto fields = water::jacobian_fields(field, choppiness, time);
    std::size_t folded = 0;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::size_t cell = z * n + x;
            const float determinant = (1.0f + fields.xx[cell]) * (1.0f + fields.zz[cell])
                                    - fields.xz[cell] * fields.xz[cell];
            const glm::vec2 p{static_cast<float>(x) * patch / n, static_cast<float>(z) * patch / n};
            INFO("at (" << x << ", " << z << ")");
            REQUIRE(determinant == Approx(water::horizontal_jacobian(water::apply_wind(gerstner, {0.0f, 0.0f}), p, time)).margin(2e-3));
            if (determinant < 0.0f) ++folded;
        }
    }
    REQUIRE(folded > 0);
}

// --- A drawn sea as waves physics can sum ---

TEST_CASE("Every wave of a sea, summed in closed form, is its transform", "[water_spectrum]") {
    // The closed-form waves are meant to be the GPU's sea written another way.
    // The transform of the evolved spectrum is that sea by a different route
    // entirely — no amplitudes, phases or travelling waves in it, only a
    // Fourier sum — so agreeing with it at every sample, at several times,
    // pins the pairing factor, the phase, the dispersion and the sign of the
    // horizontal term all at once.
    FftContributor sea = physical_sea();
    sea.resolution = 16;
    sea.patch_size = 96.0f;
    sea.wind_direction = {0.7f, -0.5f};
    const float choppiness = 1.6f;
    const auto field = water::generate_spectrum(sea);
    const auto all = water::strongest_waves(field, 16 * 16);

    // The grid's highest frequency is its own opposite and so a standing
    // ripple, which no travelling wave stands for; strongest_waves() leaves it
    // out and says how little that is. The transform it is held to leaves the
    // same cells out, so the two are the same sea.
    REQUIRE(all.height_share > 0.995f);
    REQUIRE(all.height_share < 1.0f);
    water::SpectrumField travelling = field;
    for (std::uint32_t i = 0; i < 16; ++i) {
        travelling.amplitudes[8 * 16 + i] = glm::vec2(0.0f);
        travelling.amplitudes[i * 16 + 8] = glm::vec2(0.0f);
    }

    for (float time : {0.0f, 1.3f, 17.0f}) {
        const auto spectra = water::evolve_spectrum(travelling, choppiness, time);
        const auto height = water::inverse_transform_reference(spectra.height, 16);
        const auto along_x = water::inverse_transform_reference(spectra.along_x, 16);
        const auto along_z = water::inverse_transform_reference(spectra.along_z, 16);
        for (std::uint32_t z = 0; z < 16; ++z) {
            for (std::uint32_t x = 0; x < 16; ++x) {
                const glm::vec2 p{static_cast<float>(x) * 6.0f, static_cast<float>(z) * 6.0f};
                const glm::vec3 d = water::spectral_displacement(all.waves, choppiness, p, time);
                INFO("at (" << x << ", " << z << ") t=" << time);
                REQUIRE(d.y == Approx(height[z * 16 + x]).margin(2e-4));
                REQUIRE(d.x == Approx(along_x[z * 16 + x]).margin(2e-4));
                REQUIRE(d.z == Approx(along_z[z * 16 + x]).margin(2e-4));
            }
        }
    }
}

TEST_CASE("Taking the strongest waves carries exactly the share it says", "[water_spectrum]") {
    // height_share claims the RMS height of the N-wave sea over the RMS height
    // of the whole sea, averaged over time: the waves at k and -k share a
    // spatial frequency and interfere at any one instant, and only over time
    // does Parseval hold wave by wave. Averaged, the ratio follows from the
    // two surfaces by arithmetic whatever the waves were weighted, which makes
    // this a check on the taper as well as on the pick.
    FftContributor sea = physical_sea();
    sea.resolution = 32;
    sea.patch_size = 200.0f;
    const auto field = water::generate_spectrum(sea);
    const float instants[] = {0.0f, 2.5f, 19.0f, 77.0f, 240.0f, 811.0f, 1300.0f, 2900.0f};

    for (std::uint32_t count : {4u, 20u, 80u, 300u}) {
        const auto some = water::strongest_waves(field, count);
        REQUIRE(some.waves.size() == count);
        double partial_ms = 0.0, full_ms = 0.0;
        for (float time : instants) {
            // The whole sea as drawn, standing ripples and all: the height the
            // share is a share of.
            const auto full = water::inverse_transform(
                water::evolve_spectrum(field, 1.0f, time).height, 32);
            for (std::uint32_t z = 0; z < 32; ++z) {
                for (std::uint32_t x = 0; x < 32; ++x) {
                    const glm::vec2 p{static_cast<float>(x) * 200.0f / 32.0f,
                                      static_cast<float>(z) * 200.0f / 32.0f};
                    const double h = water::spectral_displacement(some.waves, 1.0f, p, time).y;
                    partial_ms += h * h;
                    full_ms += static_cast<double>(full[z * 32 + x]) * full[z * 32 + x];
                }
            }
        }
        INFO(count << " waves");
        REQUIRE(std::sqrt(partial_ms / full_ms) == Approx(some.height_share).margin(0.01));
    }

    // Strongest first; more waves never carry less; the untapered head of a
    // shorter list is the head of a longer one.
    const auto few = water::strongest_waves(field, 20);
    const auto more = water::strongest_waves(field, 80);
    REQUIRE(more.height_share > few.height_share);
    REQUIRE(more.shortest_wavelength <= few.shortest_wavelength);
    for (std::size_t i = 0; i < 10; ++i) {
        REQUIRE(few.waves[i].k == more.waves[i].k);
        REQUIRE(few.waves[i].amplitude == more.waves[i].amplitude);
    }
    // A cut set tapers to nothing at its cutoff; a full set has no cutoff.
    REQUIRE(few.waves.back().amplitude < few.waves[9].amplitude * 0.5f);
    REQUIRE(water::strongest_waves(field, 32 * 32).waves.back().amplitude > 0.0f);

    REQUIRE(water::strongest_waves(field, 0).waves.empty());
    REQUIRE(water::strongest_waves(water::SpectrumField{}, 10).waves.empty());
}

TEST_CASE("The closed-form waves change across the sea as their transform says", "[water_spectrum]") {
    // The Jacobian entries of the closed-form waves against the same entries
    // by the other route: multiply each spectrum by i k and transform. Nothing
    // is shared between them but the spectrum.
    FftContributor sea = physical_sea();
    sea.resolution = 16;
    sea.patch_size = 96.0f;
    sea.wind_direction = {0.4f, 0.9f};
    const float choppiness = 2.2f;
    const float time = 3.4f;
    const auto field = water::generate_spectrum(sea);
    const auto all = water::strongest_waves(field, 16 * 16);

    water::SpectrumField travelling = field;   // without the standing Nyquist ripples
    for (std::uint32_t i = 0; i < 16; ++i) {
        travelling.amplitudes[8 * 16 + i] = glm::vec2(0.0f);
        travelling.amplitudes[i * 16 + 8] = glm::vec2(0.0f);
    }
    const auto fields = water::jacobian_fields(travelling, choppiness, time);

    for (std::uint32_t z = 0; z < 16; ++z) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const glm::vec2 p{static_cast<float>(x) * 6.0f, static_cast<float>(z) * 6.0f};
            const auto d = water::spectral_derivatives(all.waves, choppiness, p, time);
            INFO("at (" << x << ", " << z << ")");
            REQUIRE(d.xx == Approx(fields.xx[z * 16 + x]).margin(2e-4));
            REQUIRE(d.zz == Approx(fields.zz[z * 16 + x]).margin(2e-4));
            REQUIRE(d.xz == Approx(fields.xz[z * 16 + x]).margin(2e-4));

            // The slopes, against the height itself a hand's width either side.
            const float step = 0.05f;
            const float rise_x =
                (water::spectral_displacement(all.waves, choppiness, p + glm::vec2(step, 0), time).y
                 - water::spectral_displacement(all.waves, choppiness, p - glm::vec2(step, 0), time).y)
                / (2.0f * step);
            const float rise_z =
                (water::spectral_displacement(all.waves, choppiness, p + glm::vec2(0, step), time).y
                 - water::spectral_displacement(all.waves, choppiness, p - glm::vec2(0, step), time).y)
                / (2.0f * step);
            REQUIRE(d.dh_dx == Approx(rise_x).margin(2e-3));
            REQUIRE(d.dh_dz == Approx(rise_z).margin(2e-3));
        }
    }
}

TEST_CASE("A spectral wave carries its own length", "[water_spectrum]") {
    // |k| is kept beside k so that summing at a point takes no square root.
    // It has to be exactly length(k), and never zero.
    FftContributor sea = physical_sea();
    sea.resolution = 32;
    const auto all = water::strongest_waves(water::generate_spectrum(sea), 32 * 32);
    REQUIRE(all.waves.size() > 100);
    for (const SpectralWave& w : all.waves) {
        const float k_len = glm::length(w.k);
        REQUIRE(k_len > 0.0f);
        REQUIRE(w.k_len == k_len);
    }
}

TEST_CASE("Eight waves at a time sum to what one at a time does", "[water_spectrum]") {
    // The packed evaluators and the scalar ones are two implementations of one
    // sum and are held to each other here. They may differ by the vector
    // sine's last bit, the order eight partial sums are added in, and the
    // rounding of the angle itself — k.x - omega t is thousands of radians an
    // hour in, and a float that size is only good to a few 1e-4 rad, so the
    // two orders of computing it land a few 1e-4 apart; that is an error both
    // already have against the exact angle, not one between them. A wave
    // dropped, doubled or misaligned in the packing would miss by its whole
    // amplitude.
    FftContributor sea = physical_sea();
    sea.resolution = 64;
    sea.patch_size = 200.0f;
    const auto field = water::generate_spectrum(sea);
    for (std::uint32_t count : {1u, 7u, 8u, 9u, 100u, 257u}) {
        const auto waves = water::strongest_waves(field, count).waves;
        REQUIRE(waves.size() == count);
        const water::PackedWaves packed = water::pack_waves(waves);
        REQUIRE(packed.count == count);
        REQUIRE(packed.kx.size() == (count + 7) / 8 * 8);
        for (float time : {0.0f, 41.7f, 3600.0f}) {   // an hour in: large angles
            // The angle's own rounding: a float ulp at the largest |omega t|
            // present, times a generous constant for the sum of terms.
            float largest_angle = 0.0f;
            for (const auto& w : waves) largest_angle = std::max(largest_angle, std::abs(w.omega * time) + 200.0f * glm::length(w.k));
            const float angle_slack = largest_angle * 1.2e-7f * 4.0f;
            const float tol = (2e-6f + angle_slack) * static_cast<float>(count) + 1e-6f;
            for (float x = -90.0f; x <= 90.0f; x += 45.0f) {
                const glm::vec2 p(x, 0.3f * x + 7.0f);
                INFO(count << " waves at x=" << x << " t=" << time);
                const glm::vec3 a = water::spectral_displacement(waves, 1.7f, p, time);
                const glm::vec3 b = water::spectral_displacement(packed, 1.7f, p, time);
                REQUIRE(glm::length(a - b) < tol * 2.0f);
                const auto ma = water::spectral_motion(waves, 1.7f, p, time);
                const auto mb = water::spectral_motion(packed, 1.7f, p, time);
                REQUIRE(glm::length(ma.velocity - mb.velocity) < tol * 4.0f);
                const auto da = water::spectral_derivatives(waves, 1.7f, p, time);
                const auto db = water::spectral_derivatives(packed, 1.7f, p, time);
                REQUIRE(std::abs(da.xx - db.xx) + std::abs(da.zz - db.zz) + std::abs(da.xz - db.xz)
                        + std::abs(da.dh_dx - db.dh_dx) + std::abs(da.dh_dz - db.dh_dz)
                        < tol * 4.0f);
            }
        }
    }
}

TEST_CASE("The vector sine is the library's, to a bit, at any angle", "[water_spectrum]") {
    // Both halves of the pair the engine runs: our polynomial on every
    // platform, against this platform's library. Large angles are the case
    // that matters — a wave's angle is k.x - omega t and grows with distance
    // and time — and where a careless reduction loses a bit per turn.
    std::uint32_t state = 12345u;
    auto next = [&] { state = state * 1664525u + 1013904223u; return state; };
    for (float range : {3.2f, 100.0f, 20000.0f}) {
        float worst = 0.0f;
        for (int n = 0; n < 4000; ++n) {
            alignas(32) float a[8], s[8], c[8];
            for (float& v : a) v = (static_cast<float>(next() >> 8) / 16777216.0f * 2.0f - 1.0f) * range;
            fjell::simd::f32x8 vs, vc;
            fjell::simd::sincos(_mm256_load_ps(a), vs, vc);
            _mm256_store_ps(s, vs);
            _mm256_store_ps(c, vc);
            for (int i = 0; i < 8; ++i) {
                worst = std::max({worst, std::abs(s[i] - std::sin(a[i])), std::abs(c[i] - std::cos(a[i]))});
            }
        }
        INFO("|angle| < " << range);
        REQUIRE(worst <= 1.2e-7f);
    }
}
