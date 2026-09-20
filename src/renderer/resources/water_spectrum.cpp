#include "renderer/resources/water_spectrum.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace fjell::water {

namespace {

constexpr float GRAVITY = 9.81f;
constexpr float PI = 3.14159265358979323846f;
constexpr float TAU = 2.0f * PI;

// The peak enhancement JONSWAP measured: how much taller the peak of a
// growing sea stands than that of a fully developed one.
constexpr float PEAK_ENHANCEMENT = 3.3f;

// A uniform in (0, 1] from three integers. A hash rather than a generator
// with state so that a cell's amplitude depends only on which cell it is, and
// written out here because the standard distributions are free to differ
// between libraries — the same seed has to be the same sea on every machine.
[[nodiscard]] float unit_random(std::uint32_t x, std::uint32_t z, std::uint32_t seed,
                                std::uint32_t stream) {
    std::uint32_t h = x * 0x9E3779B1u ^ z * 0x85EBCA77u ^ seed * 0xC2B2AE3Du
                    ^ stream * 0x27D4EB2Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    // The top 24 bits, which a float holds exactly, moved off zero so the
    // logarithm below never sees it.
    return (static_cast<float>(h >> 8) + 1.0f) / 16777216.0f;
}

// A pair of independent standard normals, by Box-Muller.
[[nodiscard]] glm::vec2 gaussian_pair(std::uint32_t x, std::uint32_t z, std::uint32_t seed) {
    const float u1 = unit_random(x, z, seed, 0);
    const float u2 = unit_random(x, z, seed, 1);
    const float radius = std::sqrt(-2.0f * std::log(u1));
    return {radius * std::cos(TAU * u2), radius * std::sin(TAU * u2)};
}

// The exponent s of cos^(2s)(angle / 2) that puts half the downwind energy
// density at `spread_degrees` from the wind.
[[nodiscard]] float spread_exponent(float spread_degrees) {
    const float half_angle = glm::radians(std::clamp(spread_degrees, 1.0f, 179.0f)) * 0.5f;
    return std::log(0.5f) / (2.0f * std::log(std::cos(half_angle)));
}

} // namespace

float peak_frequency(const FftContributor& sea) {
    if (sea.wind_speed <= 0.0f || sea.fetch <= 0.0f) return 0.0f;
    return 22.0f * std::cbrt(GRAVITY * GRAVITY / (sea.wind_speed * sea.fetch));
}

float jonswap(const FftContributor& sea, float omega) {
    const float peak = peak_frequency(sea);
    if (omega <= 0.0f || peak <= 0.0f) return 0.0f;

    const float alpha = 0.076f * std::pow(sea.wind_speed * sea.wind_speed
                                          / (sea.fetch * GRAVITY), 0.22f);
    const float sigma = omega <= peak ? 0.07f : 0.09f;
    const float offset = (omega - peak) / (sigma * peak);
    const float enhancement = std::pow(PEAK_ENHANCEMENT, std::exp(-0.5f * offset * offset));

    const float ratio = peak / omega;
    return alpha * GRAVITY * GRAVITY / std::pow(omega, 5.0f)
         * std::exp(-1.25f * ratio * ratio * ratio * ratio) * enhancement;
}

float directional_spread(const FftContributor& sea, float angle_from_wind) {
    const float s = spread_exponent(sea.spread_degrees);
    // The integral of cos^(2s)(angle / 2) around the circle, in closed form,
    // which is what makes this a share of the energy rather than a gain on it.
    const float norm = std::exp(std::lgamma(s + 1.0f) - std::lgamma(s + 0.5f))
                     / (2.0f * std::sqrt(PI));
    const float c = std::abs(std::cos(angle_from_wind * 0.5f));
    return norm * std::pow(c, 2.0f * s);
}

float wavenumber_spectrum(const FftContributor& sea, glm::vec2 k) {
    const float k_len = glm::length(k);
    const float wind_len = glm::length(sea.wind_direction);
    if (k_len < 1e-6f || wind_len < 1e-6f) return 0.0f;

    // Deep water: omega^2 = g k, so a band of frequencies d(omega) is a ring
    // of wavevectors of area k dk d(angle), and the density changes by the
    // ratio of the two.
    const float omega = std::sqrt(GRAVITY * k_len);
    const float domega_dk = GRAVITY / (2.0f * omega);

    const glm::vec2 wind = sea.wind_direction / wind_len;
    const float angle = std::atan2(wind.x * k.y - wind.y * k.x, glm::dot(wind, k));

    float density = jonswap(sea, omega) * directional_spread(sea, angle) * domega_dk / k_len;

    if (sea.high_cut > 0.0f) {
        const float fade = k_len * sea.high_cut / TAU;
        density *= std::exp(-fade * fade);
    }
    return density * sea.amplitude_scale * sea.amplitude_scale;
}

float SpectrumField::wavenumber(std::uint32_t i) const {
    const auto n = static_cast<std::int32_t>(i);
    const auto size = static_cast<std::int32_t>(resolution);
    return static_cast<float>(n < size / 2 ? n : n - size) * k_spacing;
}

SpectrumField generate_spectrum(const FftContributor& sea) {
    SpectrumField field;
    if (sea.resolution < 2 || sea.patch_size <= 0.0f) return field;

    field.resolution = std::bit_floor(sea.resolution);
    field.k_spacing = TAU / sea.patch_size;
    field.amplitudes.resize(static_cast<std::size_t>(field.resolution) * field.resolution);

    const float cell_area = field.k_spacing * field.k_spacing;
    for (std::uint32_t z = 0; z < field.resolution; ++z) {
        for (std::uint32_t x = 0; x < field.resolution; ++x) {
            const glm::vec2 k{field.wavenumber(x), field.wavenumber(z)};

            // The surface is made real by adding, to every wavevector, the
            // conjugate of its opposite — which is the same physical wave
            // written a second time. A wave whose amplitude here is h so
            // stands 2|h| tall and carries a variance of 2|h|^2. For that to
            // be the cell's share of the spectrum, |h|^2 must average half of
            // it, and a pair of unit normals already averages 2.
            const float variance = wavenumber_spectrum(sea, k) * cell_area;
            field.amplitudes[static_cast<std::size_t>(z) * field.resolution + x] =
                gaussian_pair(x, z, sea.seed) * std::sqrt(variance * 0.25f);
        }
    }
    return field;
}

DisplacementSpectra evolve_spectrum(const SpectrumField& field, float choppiness, float time) {
    DisplacementSpectra out;
    const std::uint32_t n = field.resolution;
    const std::size_t count = field.amplitudes.size();
    out.height.resize(count);
    out.along_x.resize(count);
    out.along_z.resize(count);

    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::size_t here = static_cast<std::size_t>(z) * n + x;
            const std::size_t opposite = static_cast<std::size_t>((n - z) % n) * n + (n - x) % n;

            const glm::vec2 k{field.wavenumber(x), field.wavenumber(z)};
            const float k_len = glm::length(k);
            const float phase = std::sqrt(GRAVITY * k_len) * time;
            const float c = std::cos(phase);
            const float s = std::sin(phase);

            // h0(k) e^{-i w t} + conj(h0(-k)) e^{+i w t}: the wave at k moving
            // the way k points, and the wave at -k written as its conjugate.
            const glm::vec2 a = field.amplitudes[here];
            const glm::vec2 b = field.amplitudes[opposite];
            const glm::vec2 h{a.x * c + a.y * s + b.x * c + b.y * s,
                              a.y * c - a.x * s - b.y * c + b.x * s};
            out.height[here] = h;

            if (k_len < 1e-6f) continue;
            // i (k / |k|) h: a quarter cycle ahead of the height, along k,
            // which carries water toward a crest from both sides of it.
            const glm::vec2 turned{-h.y, h.x};
            out.along_x[here] = turned * (k.x / k_len * choppiness);
            out.along_z[here] = turned * (k.y / k_len * choppiness);
        }
    }
    return out;
}

std::vector<float> inverse_transform_reference(const std::vector<glm::vec2>& spectrum,
                                               std::uint32_t resolution) {
    const std::uint32_t n = resolution;
    std::vector<float> out(static_cast<std::size_t>(n) * n, 0.0f);
    if (spectrum.size() != out.size()) return out;

    constexpr double TAU_D = 6.28318530717958647692;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            double sum = 0.0;
            for (std::uint32_t kz = 0; kz < n; ++kz) {
                for (std::uint32_t kx = 0; kx < n; ++kx) {
                    // Index and wavenumber differ by a whole multiple of N
                    // in the second half, which is a whole turn of phase at
                    // every sample, so the index does for both.
                    const double angle = TAU_D * (static_cast<double>(kx) * x
                                                  + static_cast<double>(kz) * z) / n;
                    const glm::vec2 a = spectrum[static_cast<std::size_t>(kz) * n + kx];
                    sum += a.x * std::cos(angle) - a.y * std::sin(angle);
                }
            }
            out[static_cast<std::size_t>(z) * n + x] = static_cast<float>(sum);
        }
    }
    return out;
}

} // namespace fjell::water
