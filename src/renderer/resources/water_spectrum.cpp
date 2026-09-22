#include "renderer/resources/water_spectrum.hpp"

#include <algorithm>
#include <bit>
#include <utility>
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

// The Phillips constant as JONSWAP measured it: how much energy the wind has
// put into the sea, falling as the fetch grows and the sea spreads it over
// longer waves. Callers have already turned away a sea with no wind or fetch.
[[nodiscard]] float energy_scale(const FftContributor& sea) {
    return 0.076f * std::pow(sea.wind_speed * sea.wind_speed / (sea.fetch * GRAVITY), 0.22f);
}

} // namespace

namespace {

// What a sea's settings come to, worked out once. Every one of these is the
// same for all sixty-five thousand wavevectors of a grid, and between them
// they are most of what a draw would otherwise spend its time on.
struct SeaConstants {
    float peak{0.0f};          // rad/s; 0 for a sea with no wind or no fetch
    float energy{0.0f};        // the Phillips constant
    float spread_power{0.0f};  // s in cos^(2s)(angle / 2)
    float spread_norm{0.0f};   // makes the spreading integrate to 1
    glm::vec2 wind{0.0f};      // unit, or zero when the wind has no direction
};

[[nodiscard]] SeaConstants constants_of(const FftContributor& sea) {
    SeaConstants c;
    if (sea.wind_speed > 0.0f && sea.fetch > 0.0f) {
        c.peak = 22.0f * std::cbrt(GRAVITY * GRAVITY / (sea.wind_speed * sea.fetch));
        c.energy = energy_scale(sea);
    }
    c.spread_power = spread_exponent(sea.spread_degrees);
    // The integral of cos^(2s)(angle / 2) around the circle, in closed form,
    // which is what makes the spreading a share of the energy rather than a
    // gain on it.
    c.spread_norm = std::exp(std::lgamma(c.spread_power + 1.0f)
                             - std::lgamma(c.spread_power + 0.5f))
                  / (2.0f * std::sqrt(PI));
    const float wind_len = glm::length(sea.wind_direction);
    if (wind_len >= 1e-6f) c.wind = sea.wind_direction / wind_len;
    return c;
}

[[nodiscard]] float jonswap_at(const SeaConstants& c, float omega) {
    if (omega <= 0.0f || c.peak <= 0.0f) return 0.0f;

    const float sigma = omega <= c.peak ? 0.07f : 0.09f;
    const float offset = (omega - c.peak) / (sigma * c.peak);
    const float enhancement = std::pow(PEAK_ENHANCEMENT, std::exp(-0.5f * offset * offset));

    const float ratio = c.peak / omega;
    const float ratio4 = ratio * ratio * ratio * ratio;
    const float omega5 = omega * omega * omega * omega * omega;
    return c.energy * GRAVITY * GRAVITY / omega5 * std::exp(-1.25f * ratio4) * enhancement;
}

// cos^(2s)(angle / 2), from the cosine of the angle itself: cos^2 of a half
// angle is (1 + cos) / 2, which spares finding the angle at all.
[[nodiscard]] float spread_at(const SeaConstants& c, float cos_angle_from_wind) {
    const float half_angle_cos_squared = std::max(0.5f * (1.0f + cos_angle_from_wind), 0.0f);
    return c.spread_norm * std::pow(half_angle_cos_squared, c.spread_power);
}

[[nodiscard]] float density_at(const SeaConstants& c, const FftContributor& sea, glm::vec2 k) {
    const float k_len = glm::length(k);
    if (k_len < 1e-6f || c.wind == glm::vec2(0.0f)) return 0.0f;

    // Deep water: omega^2 = g k, so a band of frequencies d(omega) is a ring
    // of wavevectors of area k dk d(angle), and the density changes by the
    // ratio of the two.
    const float omega = std::sqrt(GRAVITY * k_len);
    const float domega_dk = GRAVITY / (2.0f * omega);

    float density = jonswap_at(c, omega) * spread_at(c, glm::dot(c.wind, k) / k_len)
                  * domega_dk / k_len;

    if (sea.high_cut > 0.0f) {
        const float fade = k_len * sea.high_cut / TAU;
        density *= std::exp(-fade * fade);
    }
    return density * sea.amplitude_scale * sea.amplitude_scale;
}

} // namespace

float peak_frequency(const FftContributor& sea) {
    return constants_of(sea).peak;
}

float significant_amplitude(const FftContributor& sea) {
    const SeaConstants c = constants_of(sea);
    if (c.peak <= 0.0f) return 0.0f;
    const float variance = 1.52f * c.energy * GRAVITY * GRAVITY
                         / (5.0f * c.peak * c.peak * c.peak * c.peak);
    // Significant wave height is four standard deviations, crest to trough.
    return 2.0f * std::sqrt(variance) * sea.amplitude_scale;
}

float jonswap(const FftContributor& sea, float omega) {
    return jonswap_at(constants_of(sea), omega);
}

float directional_spread(const FftContributor& sea, float angle_from_wind) {
    return spread_at(constants_of(sea), std::cos(angle_from_wind));
}

float wavenumber_spectrum(const FftContributor& sea, glm::vec2 k) {
    return density_at(constants_of(sea), sea, k);
}

float SpectrumField::wavenumber(std::uint32_t i) const {
    const auto n = static_cast<std::int32_t>(i);
    const auto size = static_cast<std::int32_t>(resolution);
    return static_cast<float>(n < size / 2 ? n : n - size) * k_spacing;
}

SpectrumNoise draw_noise(std::uint32_t seed, std::uint32_t resolution) {
    SpectrumNoise noise;
    if (resolution < 2) return noise;
    noise.resolution = std::bit_floor(resolution);
    noise.seed = seed;
    noise.normals.resize(static_cast<std::size_t>(noise.resolution) * noise.resolution);
    for (std::uint32_t z = 0; z < noise.resolution; ++z) {
        for (std::uint32_t x = 0; x < noise.resolution; ++x) {
            noise.normals[static_cast<std::size_t>(z) * noise.resolution + x] =
                gaussian_pair(x, z, seed);
        }
    }
    return noise;
}

SpectrumField generate_spectrum(const FftContributor& sea) {
    return generate_spectrum(sea, draw_noise(sea.seed, sea.resolution));
}

SpectrumField generate_spectrum(const FftContributor& sea, const SpectrumNoise& noise) {
    SpectrumField field;
    if (sea.resolution < 2 || sea.patch_size <= 0.0f) return field;
    if (noise.seed != sea.seed || noise.resolution != std::bit_floor(sea.resolution)) return field;

    field.resolution = noise.resolution;
    field.k_spacing = TAU / sea.patch_size;
    field.amplitudes.resize(noise.normals.size());

    const SeaConstants constants = constants_of(sea);
    const float cell_area = field.k_spacing * field.k_spacing;
    for (std::uint32_t z = 0; z < field.resolution; ++z) {
        for (std::uint32_t x = 0; x < field.resolution; ++x) {
            const std::size_t cell = static_cast<std::size_t>(z) * field.resolution + x;
            const glm::vec2 k{field.wavenumber(x), field.wavenumber(z)};

            // The surface is made real by adding, to every wavevector, the
            // conjugate of its opposite — which is the same physical wave
            // written a second time. A wave whose amplitude here is h so
            // stands 2|h| tall and carries a variance of 2|h|^2. For that to
            // be the cell's share of the spectrum, |h|^2 must average half of
            // it, and a pair of unit normals already averages 2.
            const float variance = density_at(constants, sea, k) * cell_area;
            field.amplitudes[cell] = noise.normals[cell] * std::sqrt(variance * 0.25f);
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

namespace {

// An in-place inverse FFT of `count` complex values `stride` apart: bit
// reversal, then the butterflies, unnormalised and turning the positive way —
// the same convention as the plain sum and as the GPU's.
void inverse_fft_line(glm::vec2* data, std::uint32_t count, std::size_t stride) {
    const auto bits = static_cast<std::uint32_t>(std::countr_zero(count));
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t reversed = 0;
        for (std::uint32_t b = 0; b < bits; ++b) reversed |= ((i >> b) & 1u) << (bits - 1 - b);
        if (reversed > i) std::swap(data[i * stride], data[reversed * stride]);
    }
    for (std::uint32_t span = 2; span <= count; span <<= 1) {
        const std::uint32_t half = span >> 1;
        for (std::uint32_t j = 0; j < half; ++j) {
            const float angle = TAU * static_cast<float>(j) / static_cast<float>(span);
            const glm::vec2 w{std::cos(angle), std::sin(angle)};
            for (std::uint32_t start = 0; start < count; start += span) {
                glm::vec2& lower = data[(start + j) * stride];
                glm::vec2& upper = data[(start + j + half) * stride];
                const glm::vec2 turned{upper.x * w.x - upper.y * w.y,
                                       upper.x * w.y + upper.y * w.x};
                upper = lower - turned;
                lower = lower + turned;
            }
        }
    }
}

} // namespace

std::vector<float> inverse_transform(const std::vector<glm::vec2>& spectrum,
                                     std::uint32_t resolution) {
    const std::uint32_t n = resolution;
    std::vector<float> out(static_cast<std::size_t>(n) * n, 0.0f);
    if (n < 2 || !std::has_single_bit(n) || spectrum.size() != out.size()) return out;

    std::vector<glm::vec2> work = spectrum;
    for (std::uint32_t z = 0; z < n; ++z) {
        inverse_fft_line(work.data() + static_cast<std::size_t>(z) * n, n, 1);
    }
    for (std::uint32_t x = 0; x < n; ++x) inverse_fft_line(work.data() + x, n, n);

    for (std::size_t i = 0; i < out.size(); ++i) out[i] = work[i].x;
    return out;
}

JacobianFields jacobian_fields(const SpectrumField& field, float choppiness, float time) {
    const DisplacementSpectra spectra = evolve_spectrum(field, choppiness, time);
    const std::uint32_t n = field.resolution;

    // A derivative along an axis is a multiplication by i k along it.
    auto derivative = [&](const std::vector<glm::vec2>& of, bool along_x) {
        std::vector<glm::vec2> out(of.size());
        for (std::uint32_t z = 0; z < n; ++z) {
            for (std::uint32_t x = 0; x < n; ++x) {
                const std::size_t cell = static_cast<std::size_t>(z) * n + x;
                const float k = field.wavenumber(along_x ? x : z);
                out[cell] = glm::vec2(-of[cell].y, of[cell].x) * k;
            }
        }
        return out;
    };

    JacobianFields fields;
    fields.xx = inverse_transform(derivative(spectra.along_x, true), n);
    fields.zz = inverse_transform(derivative(spectra.along_z, false), n);
    fields.xz = inverse_transform(derivative(spectra.along_x, false), n);
    return fields;
}

StrongestWaves strongest_waves(const SpectrumField& field, std::uint32_t count) {
    StrongestWaves out;
    const std::uint32_t n = field.resolution;

    // A stored amplitude h makes a wave 2|h| tall carrying a variance of
    // 2|h|^2, once it has been paired with the conjugate of its opposite.
    struct Cell {
        float variance;
        std::uint32_t index;
    };
    std::vector<Cell> cells;
    cells.reserve(field.amplitudes.size());
    double total = 0.0;
    for (std::uint32_t i = 0; i < field.amplitudes.size(); ++i) {
        const glm::vec2 a = field.amplitudes[i];
        const float variance = 2.0f * (a.x * a.x + a.y * a.y);
        if (variance <= 0.0f) continue;
        total += static_cast<double>(variance);
        // The grid's highest frequency along either axis is its own opposite:
        // there +k and -k are one cell, so what the simulation makes of it is
        // a ripple standing in place rather than a wave going anywhere, and
        // no travelling wave stands for it. It counts toward the sea's height
        // and is never taken. These are the shortest waves the grid holds,
        // the last a floating body would be given in any case.
        if (i % n == n / 2 || i / n == n / 2) continue;
        cells.push_back({variance, i});
    }
    if (cells.empty() || count == 0) return out;

    const std::size_t taken = std::min<std::size_t>(count, cells.size());
    // By strength, and by place in the grid where strengths tie, so the order
    // is the spectrum's and not the sort's.
    auto stronger = [](const Cell& a, const Cell& b) {
        return a.variance != b.variance ? a.variance > b.variance : a.index < b.index;
    };
    std::partial_sort(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(taken),
                      cells.end(), stronger);

    out.waves.reserve(taken);
    double carried = 0.0;
    float largest_k = 0.0f;
    for (std::size_t i = 0; i < taken; ++i) {
        const glm::vec2 a = field.amplitudes[cells[i].index];
        SpectralWave wave;
        wave.k = {field.wavenumber(cells[i].index % n), field.wavenumber(cells[i].index / n)};
        const float k_len = glm::length(wave.k);
        wave.amplitude = 2.0f * glm::length(a);
        wave.phase = std::atan2(a.y, a.x);
        wave.omega = std::sqrt(GRAVITY * k_len);
        out.waves.push_back(wave);
        carried += static_cast<double>(cells[i].variance);
        largest_k = std::max(largest_k, k_len);
    }
    out.height_share = static_cast<float>(std::sqrt(carried / total));
    out.shortest_wavelength = largest_k > 0.0f ? TAU / largest_k : 0.0f;
    return out;
}

glm::vec3 spectral_displacement(const std::vector<SpectralWave>& waves, float choppiness,
                                glm::vec2 world_xz, float time) {
    glm::vec3 sum{0.0f};
    for (const SpectralWave& wave : waves) {
        const float k_len = glm::length(wave.k);
        if (k_len < 1e-6f) continue;
        const float angle = glm::dot(wave.k, world_xz) - wave.omega * time + wave.phase;
        sum.y += wave.amplitude * std::cos(angle);
        // A quarter cycle ahead of the height, along k: toward the crest from
        // both sides of it, as evolve_spectrum() has it.
        const float pinch = -wave.amplitude * std::sin(angle) * choppiness / k_len;
        sum.x += wave.k.x * pinch;
        sum.z += wave.k.y * pinch;
    }
    return sum;
}

SpectralMotion spectral_motion(const std::vector<SpectralWave>& waves, float choppiness,
                               glm::vec2 world_xz, float time) {
    SpectralMotion m;
    for (const SpectralWave& wave : waves) {
        const float k_len = glm::length(wave.k);
        if (k_len < 1e-6f) continue;
        const float angle = glm::dot(wave.k, world_xz) - wave.omega * time + wave.phase;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        // The angle falls at omega, so d/dt cos = omega sin, d/dt sin = -omega cos.
        m.displacement.y += wave.amplitude * c;
        m.velocity.y += wave.amplitude * s * wave.omega;
        const float pinch = -wave.amplitude * s * choppiness / k_len;
        const float pinch_rate = wave.amplitude * c * choppiness / k_len * wave.omega;
        m.displacement.x += wave.k.x * pinch;
        m.displacement.z += wave.k.y * pinch;
        m.velocity.x += wave.k.x * pinch_rate;
        m.velocity.z += wave.k.y * pinch_rate;
    }
    return m;
}

SpectralDerivatives spectral_derivatives(const std::vector<SpectralWave>& waves,
                                         float choppiness, glm::vec2 world_xz, float time) {
    SpectralDerivatives sum;
    for (const SpectralWave& wave : waves) {
        const float k_len = glm::length(wave.k);
        if (k_len < 1e-6f) continue;
        const float angle = glm::dot(wave.k, world_xz) - wave.omega * time + wave.phase;
        const float a_sin = wave.amplitude * std::sin(angle);
        // The pinch is -(k / |k|) c A sin(angle), so across the plane it
        // changes as -(k k^T / |k|) c A cos(angle).
        const float spread = -wave.amplitude * std::cos(angle) * choppiness / k_len;

        sum.dh_dx -= wave.k.x * a_sin;
        sum.dh_dz -= wave.k.y * a_sin;
        sum.xx += wave.k.x * wave.k.x * spread;
        sum.zz += wave.k.y * wave.k.y * spread;
        sum.xz += wave.k.x * wave.k.y * spread;
    }
    return sum;
}

} // namespace fjell::water
