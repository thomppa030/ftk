#include "renderer/resources/water_surface.hpp"

#include <algorithm>
#include <cmath>

namespace fjell::water {

namespace {

// Earth gravity, the constant behind deep-water wave speed. Matches
// FJELL_WATER_GRAVITY in fjell_water_waves.glsl.
constexpr float GRAVITY = 9.81f;

constexpr float TAU = 6.28318530717958647692f;

// Steepness is authored 0..1, but the Gerstner horizontal term wants Q, and Q
// past 1/(k*A) folds the surface back through itself. Convert here so the
// asset's dial cannot produce a broken surface however it is set, and so both
// evaluators share the definition.
[[nodiscard]] float steepness_to_q(float steepness, float k, float amplitude) {
    float max_q = 1.0f / std::max(k * amplitude, 1e-6f);
    return std::clamp(steepness, 0.0f, 1.0f) * max_q;
}

struct WaveTerms {
    glm::vec2 dir{1.0f, 0.0f};
    float k{0.0f};          // angular wavenumber
    float amplitude{0.0f};
    float q{0.0f};          // horizontal bunching
    float phase{0.0f};      // dot(dir, xz) * k - omega * t
};

// Everything both the displacement and its derivatives need, computed once.
[[nodiscard]] WaveTerms terms_for(const WaterWave& wave, const FjWaterData& water,
                                  glm::vec2 world_xz, float time) {
    WaveTerms t;
    float len = glm::length(wave.direction);
    if (len < 1e-6f || wave.wavelength < 1e-4f) {
        return t;  // amplitude 0: contributes nothing
    }
    t.dir = wave.direction / len;
    t.k = TAU / wave.wavelength;
    t.amplitude = wave.amplitude * water.amplitude_scale;
    t.q = steepness_to_q(wave.steepness, t.k, t.amplitude);

    float omega = std::sqrt(GRAVITY * t.k) * wave.speed * water.speed_scale;
    t.phase = glm::dot(t.dir, world_xz) * t.k - omega * time;
    return t;
}

} // namespace

float wave_frequency(float wavelength) {
    if (wavelength < 1e-4f) return 0.0f;
    return std::sqrt(GRAVITY * (TAU / wavelength));
}

glm::vec3 displace(const FjWaterData& water, glm::vec2 world_xz, float time) {
    glm::vec3 sum{0.0f};
    std::size_t count = std::min<std::size_t>(water.waves.size(), MAX_WATER_WAVES);
    for (std::size_t i = 0; i < count; ++i) {
        WaveTerms t = terms_for(water.waves[i], water, world_xz, time);
        if (t.amplitude == 0.0f) continue;

        float c = std::cos(t.phase);
        float s = std::sin(t.phase);
        // Horizontal bunching toward the crest, then the crest itself.
        sum.x += t.q * t.amplitude * t.dir.x * c;
        sum.z += t.q * t.amplitude * t.dir.y * c;
        sum.y += t.amplitude * s;
    }
    return sum;
}

glm::vec3 normal_at(const FjWaterData& water, glm::vec2 world_xz, float time) {
    // Partial derivatives of the displaced surface with respect to the rest
    // position. The normal is the cross product of the two tangents; writing
    // the sums out avoids building both tangents separately.
    glm::vec3 binormal{1.0f, 0.0f, 0.0f};  // d/dx
    glm::vec3 tangent{0.0f, 0.0f, 1.0f};   // d/dz

    std::size_t count = std::min<std::size_t>(water.waves.size(), MAX_WATER_WAVES);
    for (std::size_t i = 0; i < count; ++i) {
        WaveTerms t = terms_for(water.waves[i], water, world_xz, time);
        if (t.amplitude == 0.0f) continue;

        float c = std::cos(t.phase);
        float s = std::sin(t.phase);
        float wa = t.k * t.amplitude;

        binormal.x -= t.q * t.dir.x * t.dir.x * wa * s;
        binormal.y += t.dir.x * wa * c;
        binormal.z -= t.q * t.dir.x * t.dir.y * wa * s;

        tangent.x -= t.q * t.dir.x * t.dir.y * wa * s;
        tangent.y += t.dir.y * wa * c;
        tangent.z -= t.q * t.dir.y * t.dir.y * wa * s;
    }

    glm::vec3 n = glm::cross(tangent, binormal);
    float len = glm::length(n);
    if (len < 1e-6f) return glm::vec3(0.0f, 1.0f, 0.0f);
    return n / len;
}

float crest(const FjWaterData& water, glm::vec2 world_xz, float time) {
    // The Jacobian of the horizontal displacement. At rest it is 1; as trains
    // bunch together it falls, reaching 0 where the surface has pinched to a
    // point and going negative where it would fold. Map that to 0..1 rising
    // into the crest so shaders can use it directly as a foam mask.
    float jxx = 1.0f;
    float jzz = 1.0f;
    float jxz = 0.0f;

    std::size_t count = std::min<std::size_t>(water.waves.size(), MAX_WATER_WAVES);
    for (std::size_t i = 0; i < count; ++i) {
        WaveTerms t = terms_for(water.waves[i], water, world_xz, time);
        if (t.amplitude == 0.0f) continue;

        float s = std::sin(t.phase);
        float wa = t.k * t.amplitude * t.q;

        jxx -= wa * t.dir.x * t.dir.x * s;
        jzz -= wa * t.dir.y * t.dir.y * s;
        jxz -= wa * t.dir.x * t.dir.y * s;
    }

    float jacobian = jxx * jzz - jxz * jxz;
    return std::clamp(1.0f - jacobian, 0.0f, 1.0f);
}

float height_at(const FjWaterData& water, glm::vec2 world_xz, float time) {
    return displace(water, world_xz, time).y;
}

glm::vec2 rest_position_for(const FjWaterData& water, glm::vec2 world_xz, float time) {
    // Gerstner moves a vertex horizontally, so the point now above world_xz
    // started somewhere else. Solve for the rest position whose displacement
    // lands on world_xz.
    //
    // Newton rather than the obvious fixed-point walk (rest = target -
    // displacement): that walk converges linearly and, on a steep sea, barely
    // at all — measured at steepness 0.9 it was still 0.38 m out after eight
    // passes. The horizontal Jacobian is already needed for crest() and makes
    // this quadratic instead.
    //
    // Quadratic once it is near a solution, which is not the same as always.
    // Over a column the surface stands above more than once, the first steps
    // can throw the guess further out before it settles — measured on a
    // steepness-0.9 sea, error at one sample went 8.8, 1.1, 3.0, 0.44, 0.04.
    // So iterate to a tolerance with a ceiling rather than a fixed count, and
    // spend the steps only where they are needed.
    constexpr float TOLERANCE = 1e-3f;      // a millimetre of surface
    constexpr int MAX_STEPS = 8;

    glm::vec2 rest = world_xz;
    for (int i = 0; i < MAX_STEPS; ++i) {
        glm::vec3 d = displace(water, rest, time);
        glm::vec2 error = rest + glm::vec2(d.x, d.z) - world_xz;
        if (glm::dot(error, error) < TOLERANCE * TOLERANCE) break;

        // d(displaced)/d(rest), the same terms crest() reduces to a scalar.
        float jxx = 1.0f;
        float jzz = 1.0f;
        float jxz = 0.0f;
        std::size_t count = std::min<std::size_t>(water.waves.size(), MAX_WATER_WAVES);
        for (std::size_t w = 0; w < count; ++w) {
            WaveTerms t = terms_for(water.waves[w], water, rest, time);
            if (t.amplitude == 0.0f) continue;
            float s = std::sin(t.phase);
            float wa = t.k * t.amplitude * t.q;
            jxx -= wa * t.dir.x * t.dir.x * s;
            jzz -= wa * t.dir.y * t.dir.y * s;
            jxz -= wa * t.dir.x * t.dir.y * s;
        }

        float det = jxx * jzz - jxz * jxz;
        // A vanishing determinant is the surface pinched to a point, where
        // there is no single rest position to solve for. Stop rather than
        // divide through it; the current guess is as good as it gets.
        if (std::abs(det) < 1e-6f) break;

        rest -= glm::vec2(( jzz * error.x - jxz * error.y) / det,
                          (-jxz * error.x + jxx * error.y) / det);
    }
    return rest;
}

float height_at_iterative(const FjWaterData& water, glm::vec2 world_xz, float time) {
    return displace(water, rest_position_for(water, world_xz, time), time).y;
}

} // namespace fjell::water
