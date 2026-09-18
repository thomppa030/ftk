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
[[nodiscard]] float steepness_to_q(float steepness, float k, float amplitude,
                                   float budget) {
    float max_q = 1.0f / std::max(k * amplitude, 1e-6f);
    return std::clamp(steepness, 0.0f, 1.0f) * max_q * budget;
}

// How much of a single train's fold limit each train may spend.
//
// A Gerstner train folds when k*A*q reaches 1, which steepness_to_q caps it
// at, so steepness 1 is that train exactly at its own limit and nothing here
// scales it down for having company.
//
// It used to. The budget was shared 1/N, on the reasoning that trains sum and
// the surface folds on the sum, so N trains each at the single-train limit
// could reach N. That bound is real but it is the worst case — every crest
// aligned, at one instant, at one point — and trains travelling at different
// speeds, angles and wavelengths do not arrange that. Designing to it cost
// the sea the pinch that is the whole reason for choosing Gerstner over a sum
// of sines: on a six-train sea it held every train below an eighth of its own
// steepness, and dragging every slider to maximum still could not produce a
// sharp crest. Worse, dividing by the count meant adding a small train for
// surface detail flattened every train already there, which is backwards from
// how a sea is authored.
//
// So steepness now means what the asset always claimed it meant, and whether
// a particular field folds is measured rather than prevented — the editor
// samples the surface and reports the percentage, and horizontal_jacobian()
// is what anything else asks. `headroom` scales the whole field past a single
// train's limit for a sea that wants to fold somewhere on purpose.
[[nodiscard]] float steepness_budget(float headroom) {
    return headroom;
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
                                  glm::vec2 world_xz, float time, float budget) {
    WaveTerms t;
    float len = glm::length(wave.direction);
    if (len < 1e-6f || wave.wavelength < 1e-4f) {
        return t;  // amplitude 0: contributes nothing
    }
    t.dir = wave.direction / len;
    t.k = TAU / wave.wavelength;
    t.amplitude = wave.amplitude * water.amplitude_scale;
    t.q = steepness_to_q(wave.steepness, t.k, t.amplitude, budget);

    float omega = std::sqrt(GRAVITY * t.k) * wave.speed * water.speed_scale;
    t.phase = glm::dot(t.dir, world_xz) * t.k - omega * time;
    return t;
}

} // namespace

float wave_frequency(float wavelength) {
    if (wavelength < 1e-4f) return 0.0f;
    return std::sqrt(GRAVITY * (TAU / wavelength));
}

glm::vec2 advance_sea_wind(glm::vec2 sea_wind, glm::vec2 blowing, float dt) {
    float seconds = glm::length(blowing) > glm::length(sea_wind) ? SEA_RISE_SECONDS
                                                                 : SEA_FALL_SECONDS;
    float alpha = 1.0f - std::exp(-dt / std::max(seconds, 1e-3f));
    return sea_wind + (blowing - sea_wind) * alpha;
}

float bed_agitation(const FjWaterData& water, float depth) {
    if (water.waves.empty() || depth <= 0.0f) return 0.0f;

    // Each train reaches its own distance down. The orbital motion under a
    // wave dies as exp(-k*d), so wavelength decides the reach and amplitude
    // decides how hard it works when it gets there — a long swell stirs a bed
    // that a chop of the same height cannot feel at all.
    float stirred = 0.0f;
    for (const auto& wave : water.waves) {
        if (wave.wavelength < 1e-4f) continue;
        const float k = TAU / wave.wavelength;
        const float amplitude = wave.amplitude * water.amplitude_scale;
        stirred += amplitude * std::exp(-k * depth);
    }

    // Quoted against the depth itself rather than an absolute: half a metre of
    // orbital motion is nothing under thirty metres of water and is the whole
    // water column in a one-metre shallow. Saturating means a bed being
    // worked hard is being worked hard, however much harder it could be.
    return std::clamp(stirred / depth, 0.0f, 1.0f);
}

float advance_silt(float load, float agitation, float dt) {
    const float target = std::clamp(agitation, 0.0f, 1.0f);
    // Stirring up and settling out are different processes on very different
    // clocks, not one rate run in two directions: waves lift sediment in
    // minutes and gravity returns it over hours.
    const float seconds = target > load ? SILT_STIR_SECONDS : SILT_SETTLE_SECONDS;
    const float alpha = 1.0f - std::exp(-dt / std::max(seconds, 1e-3f));
    return load + (target - load) * alpha;
}

FjWaterData apply_wind(const FjWaterData& water, glm::vec2 wind_xz) {
    const WaterWindResponse& wind = water.wind;
    if (!wind.enabled || water.waves.empty()) {
        return water;
    }

    float reference = std::max(wind.reference_speed, 0.1f);
    float speed = glm::length(wind_xz);

    // Every response is quoted at twice the reference wind, so this is how far
    // along that span the weather currently sits: 0 at the reference, 1 at
    // double it, negative below it. Not clamped above — a storm past double
    // keeps building the sea rather than hitting a ceiling.
    float t = speed / reference - 1.0f;

    // Each response is a straight ramp through the reference point, floored at
    // zero so a calm can flatten a sea but never invert it. A dead calm does
    // not necessarily flatten it: at the default response a windless sea is
    // still 40% of its authored height, which is right — a swell outlives the
    // wind that raised it by days, and water that went glassy the moment the
    // wind dropped would read as a switch being thrown. An artist who wants a
    // millpond in a calm sets the response past 2.
    FjWaterData out = water;
    out.amplitude_scale *= std::max(1.0f + t * (wind.amplitude_response - 1.0f), 0.0f);
    out.steepness_headroom *= std::max(1.0f + t * (wind.steepness_response - 1.0f), 0.0f);

    if (speed < 1e-4f || wind.direction_response <= 0.0f) {
        // No wind is no direction to turn onto, and the sea keeps the heading
        // it was authored with.
        return out;
    }
    glm::vec2 wind_dir = wind_xz / speed;

    // Only the short trains turn. Find the span of wavelengths present so the
    // ramp is relative to this sea rather than to an absolute length that
    // would mean different things in a pond and an ocean.
    float shortest = water.waves.front().wavelength;
    float longest = shortest;
    for (const auto& w : water.waves) {
        shortest = std::min(shortest, w.wavelength);
        longest = std::max(longest, w.wavelength);
    }

    float span = longest - shortest;
    for (auto& w : out.waves) {
        float len = glm::length(w.direction);
        if (len < 1e-6f) continue;

        // 0 on the longest train, 1 on the shortest. A sea whose trains are
        // all one length has no swell to distinguish from chop, so it turns
        // as one.
        float shortness = (span < 1e-4f) ? 1.0f : (longest - w.wavelength) / span;
        float turn = std::clamp(wind.direction_response * shortness, 0.0f, 1.0f);

        // Turn toward the wind rather than snapping to it, and normalize:
        // a train half-turned onto a wind blowing straight back at it would
        // otherwise shrink to nothing through the midpoint.
        glm::vec2 turned = glm::mix(w.direction / len, wind_dir, turn);
        float turned_len = glm::length(turned);
        // Exactly opposed at exactly half a turn is the one case with no
        // answer; leave that train as it was authored.
        if (turned_len < 1e-4f) continue;
        w.direction = turned / turned_len;
    }
    return out;
}

glm::vec3 displace(const FjWaterData& water, glm::vec2 world_xz, float time) {
    glm::vec3 sum{0.0f};
    std::size_t count = std::min<std::size_t>(water.waves.size(), MAX_WATER_WAVES);
    float budget = steepness_budget(water.steepness_headroom);
    for (std::size_t i = 0; i < count; ++i) {
        WaveTerms t = terms_for(water.waves[i], water, world_xz, time, budget);
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
    float budget = steepness_budget(water.steepness_headroom);
    for (std::size_t i = 0; i < count; ++i) {
        WaveTerms t = terms_for(water.waves[i], water, world_xz, time, budget);
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

float horizontal_jacobian(const FjWaterData& water, glm::vec2 world_xz, float time) {
    // At rest it is 1; as trains bunch together it falls, reaching 0 where the
    // surface has pinched to a point and going negative where it has folded.
    float jxx = 1.0f;
    float jzz = 1.0f;
    float jxz = 0.0f;

    std::size_t count = std::min<std::size_t>(water.waves.size(), MAX_WATER_WAVES);
    float budget = steepness_budget(water.steepness_headroom);
    for (std::size_t i = 0; i < count; ++i) {
        WaveTerms t = terms_for(water.waves[i], water, world_xz, time, budget);
        if (t.amplitude == 0.0f) continue;

        float s = std::sin(t.phase);
        float wa = t.k * t.amplitude * t.q;

        jxx -= wa * t.dir.x * t.dir.x * s;
        jzz -= wa * t.dir.y * t.dir.y * s;
        jxz -= wa * t.dir.x * t.dir.y * s;
    }

    return jxx * jzz - jxz * jxz;
}

float crest(const FjWaterData& water, glm::vec2 world_xz, float time) {
    // Mapped to 0..1 rising into the crest so shaders can use it directly as a
    // foam mask. The clamp means a folded surface reads the same as one
    // pinched exactly to a point; horizontal_jacobian() keeps the difference.
    return std::clamp(1.0f - horizontal_jacobian(water, world_xz, time), 0.0f, 1.0f);
}

glm::vec2 dominant_direction(const FjWaterData& water) {
    glm::vec2 sum(0.0f);
    std::size_t count = std::min<std::size_t>(water.waves.size(), MAX_WATER_WAVES);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& w = water.waves[i];
        float len = glm::length(w.direction);
        if (len < 1e-6f || w.wavelength < 1e-4f) continue;
        sum += (w.direction / len) * w.amplitude;
    }
    float len = glm::length(sum);
    return len < 1e-6f ? glm::vec2(0.0f) : sum / len;
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
        float budget = steepness_budget(water.steepness_headroom);
        for (std::size_t w = 0; w < count; ++w) {
            WaveTerms t = terms_for(water.waves[w], water, rest, time, budget);
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
