#include "renderer/resources/water_surface.hpp"

#include "renderer/resources/water_spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <variant>

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
    float omega{0.0f};      // angular frequency, after the speed scales
    float phase{0.0f};      // dot(dir, xz) * k - omega * t
};

// Everything both the displacement and its derivatives need, computed once.
// Only ever handed a train that moves the surface, which is what lets it
// divide by the direction's length and the wavelength without asking.
[[nodiscard]] WaveTerms terms_for(const WaterWave& wave, const GerstnerContributor& contributor,
                                  glm::vec2 world_xz, float time) {
    WaveTerms t;
    t.dir = wave.direction / glm::length(wave.direction);
    t.k = TAU / wave.wavelength;
    t.amplitude = wave.amplitude * contributor.amplitude_scale;
    t.q = steepness_to_q(wave.steepness, t.k, t.amplitude,
                         steepness_budget(contributor.steepness_headroom));

    t.omega = std::sqrt(GRAVITY * t.k) * wave.speed * contributor.speed_scale;
    t.phase = glm::dot(t.dir, world_xz) * t.k - t.omega * time;
    return t;
}

// d(displaced)/d(rest) of the horizontal displacement, as the three distinct
// entries of a symmetric 2x2.
struct HorizontalJacobian {
    float xx{1.0f};
    float zz{1.0f};
    float xz{0.0f};

    [[nodiscard]] float determinant() const { return xx * zz - xz * xz; }
};

// The Gerstner contributors' share alone, for whoever works the simulated
// sea's share out another way.
[[nodiscard]] HorizontalJacobian gerstner_jacobian_terms(const FjWaterData& water,
                                                         glm::vec2 world_xz, float time) {
    // At rest it is the identity; as trains bunch together the diagonal falls.
    HorizontalJacobian j;
    for_each_summed_train(water, [&](std::uint32_t, const GerstnerContributor& contributor,
                                     const WaterWave& wave) {
        WaveTerms t = terms_for(wave, contributor, world_xz, time);
        if (t.amplitude == 0.0f) return;

        float s = std::sin(t.phase);
        float wa = t.k * t.amplitude * t.q;

        j.xx -= wa * t.dir.x * t.dir.x * s;
        j.zz -= wa * t.dir.y * t.dir.y * s;
        j.xz -= wa * t.dir.x * t.dir.y * s;
    });
    return j;
}

// Everything the CPU sums: the Gerstner contributors, and the simulated sea's
// strongest waves where the field has been given them.
[[nodiscard]] HorizontalJacobian jacobian_terms(const FjWaterData& water, glm::vec2 world_xz,
                                                float time) {
    HorizontalJacobian j = gerstner_jacobian_terms(water, world_xz, time);
    if (water.simulated.has_value()) {
        const SpectralDerivatives d = spectral_derivatives(
            water.simulated->waves, water.simulated->choppiness, world_xz, time);
        j.xx += d.xx;
        j.zz += d.zz;
        j.xz += d.xz;
    }
    return j;
}

} // namespace

const FftContributor* simulated_sea(const FjWaterData& water) {
    for (const auto& contributor : water.contributors) {
        if (const auto* fft = std::get_if<FftContributor>(&contributor)) return fft;
    }
    return nullptr;
}

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
    if (depth <= 0.0f) return 0.0f;

    // Each train reaches its own distance down. The orbital motion under a
    // wave dies as exp(-k*d), so wavelength decides the reach and amplitude
    // decides how hard it works when it gets there — a long swell stirs a bed
    // that a chop of the same height cannot feel at all.
    float stirred = 0.0f;
    for_each_summed_train(water, [&](std::uint32_t, const GerstnerContributor& contributor,
                                     const WaterWave& wave) {
        const float k = TAU / wave.wavelength;
        const float amplitude = wave.amplitude * contributor.amplitude_scale;
        stirred += amplitude * std::exp(-k * depth);
    });

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

GerstnerContributor apply_wind(const GerstnerContributor& water, glm::vec2 wind_xz) {
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
    GerstnerContributor out = water;
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

FftContributor apply_wind(const FftContributor& water, glm::vec2 wind_xz) {
    if (!water.answers_weather) return water;

    // A spectrum is a description of the sea a wind raises, so answering the
    // wind is being described by a different one. Nothing is scaled or turned
    // by hand the way a Gerstner contributor's trains are: more wind is a
    // taller, longer sea because that is what the spectrum says it is.
    // In steps, not continuously: a tenth of a metre a second and half a
    // degree, far under what shows on the water. The lagged wind drifts every
    // frame, and a sea that was a different sea every frame would have to be
    // drawn again every frame — by the GPU's spectrum and by the waves physics
    // sums alike. Stepping it here, from the wind alone, also means every
    // machine given the same wind steps at the same moment and floats its boats
    // on the same sea, which a rule comparing against "what was drawn last"
    // could not promise.
    constexpr float SPEED_STEP = 0.1f;
    constexpr float HEADING_STEP_DEGREES = 0.5f;

    FftContributor out = water;
    const float speed = glm::length(wind_xz);
    out.wind_speed = std::round(speed / SPEED_STEP) * SPEED_STEP;
    // A dead calm has no heading to take. The sea it leaves is flat whichever
    // way it faces, so it keeps the one it had for the wind to come back to.
    if (speed >= 1e-3f) {
        const float heading = glm::degrees(std::atan2(wind_xz.y, wind_xz.x));
        const float stepped = glm::radians(std::round(heading / HEADING_STEP_DEGREES)
                                           * HEADING_STEP_DEGREES);
        out.wind_direction = {std::cos(stepped), std::sin(stepped)};
    }
    return out;
}

FftContributor spectrum_settings(FftContributor sea) {
    sea.choppiness = 0.0f;
    sea.physics_waves = 0;
    return sea;
}

void retune(FjWaterData::SimulatedSea& sea, const FftContributor& now) {
    // A ceiling on what physics may be asked to sum per point, well past where
    // more waves buy anything a floating body could feel.
    constexpr std::uint32_t MAX_PHYSICS_WAVES = 1024;
    const std::uint32_t count = std::min(now.physics_waves, MAX_PHYSICS_WAVES);

    sea.choppiness = now.choppiness;
    if (sea.waves.size() != count || sea.drawn_from.physics_waves != now.physics_waves) {
        sea.waves = count > 0 && sea.spectrum != nullptr
            ? strongest_waves(*sea.spectrum, count).waves
            : std::vector<SpectralWave>{};
    }
    sea.drawn_from.choppiness = now.choppiness;
    sea.drawn_from.physics_waves = now.physics_waves;
}

FjWaterData::SimulatedSea draw_simulated_sea(const FftContributor& sea,
                                              const SpectrumNoise* noise) {
    FjWaterData::SimulatedSea drawn;
    drawn.drawn_from = sea;
    drawn.drawn_from.physics_waves = 0;   // retune() takes the waves and says so
    drawn.spectrum = std::make_shared<const SpectrumField>(
        noise != nullptr ? generate_spectrum(sea, *noise) : generate_spectrum(sea));
    retune(drawn, sea);
    return drawn;
}

FjWaterData apply_wind(const FjWaterData& water, glm::vec2 wind_xz) {
    FjWaterData out;
    out.preview_material = water.preview_material;
    out.contributors.reserve(water.contributors.size());
    for (const auto& contributor : water.contributors) {
        // Each kind has its own overload, so a kind added to the variant
        // without an answer to the wind does not compile.
        out.contributors.emplace_back(std::visit(
            [&](const auto& kind) -> WaterContributor { return apply_wind(kind, wind_xz); },
            contributor));
    }
    return out;
}

FjWaterData derive_effective(const FjWaterData& authored, glm::vec2 wind_xz,
                             const FjWaterData* previous) {
    FjWaterData out = apply_wind(authored, wind_xz);
    const FftContributor* sea = simulated_sea(out);
    if (sea == nullptr) return out;

    const bool still_stands = previous != nullptr && previous->simulated.has_value()
        && spectrum_settings(previous->simulated->drawn_from) == spectrum_settings(*sea);
    if (still_stands) {
        out.simulated = previous->simulated;
        retune(*out.simulated, *sea);
    } else {
        out.simulated = draw_simulated_sea(*sea);
    }
    return out;
}

SurfaceMotion surface_motion(const FjWaterData& water, glm::vec2 world_xz, float time) {
    SurfaceMotion m;
    for_each_summed_train(water, [&](std::uint32_t, const GerstnerContributor& contributor,
                                     const WaterWave& wave) {
        WaveTerms t = terms_for(wave, contributor, world_xz, time);
        if (t.amplitude == 0.0f) return;

        float c = std::cos(t.phase);
        float s = std::sin(t.phase);
        // The same terms displace() sums, and their time derivatives: the
        // phase falls at omega, so cos becomes omega sin and sin becomes
        // -omega cos.
        m.displacement.x += t.q * t.amplitude * t.dir.x * c;
        m.displacement.z += t.q * t.amplitude * t.dir.y * c;
        m.displacement.y += t.amplitude * s;
        m.velocity.x += t.q * t.amplitude * t.dir.x * s * t.omega;
        m.velocity.z += t.q * t.amplitude * t.dir.y * s * t.omega;
        m.velocity.y -= t.amplitude * c * t.omega;
    });
    if (water.simulated.has_value()) {
        const SpectralMotion s = spectral_motion(water.simulated->waves,
                                                 water.simulated->choppiness, world_xz, time);
        m.displacement += s.displacement;
        m.velocity += s.velocity;
    }
    return m;
}

glm::vec3 displace(const FjWaterData& water, glm::vec2 world_xz, float time) {
    glm::vec3 sum{0.0f};
    for_each_summed_train(water, [&](std::uint32_t, const GerstnerContributor& contributor,
                                     const WaterWave& wave) {
        WaveTerms t = terms_for(wave, contributor, world_xz, time);
        if (t.amplitude == 0.0f) return;

        float c = std::cos(t.phase);
        float s = std::sin(t.phase);
        // Horizontal bunching toward the crest, then the crest itself.
        sum.x += t.q * t.amplitude * t.dir.x * c;
        sum.z += t.q * t.amplitude * t.dir.y * c;
        sum.y += t.amplitude * s;
    });
    if (water.simulated.has_value()) {
        sum += spectral_displacement(water.simulated->waves,
                                     water.simulated->choppiness, world_xz, time);
    }
    return sum;
}

glm::vec3 normal_at(const FjWaterData& water, glm::vec2 world_xz, float time) {
    // Partial derivatives of the displaced surface with respect to the rest
    // position. The normal is the cross product of the two tangents; writing
    // the sums out avoids building both tangents separately.
    glm::vec3 binormal{1.0f, 0.0f, 0.0f};  // d/dx
    glm::vec3 tangent{0.0f, 0.0f, 1.0f};   // d/dz

    for_each_summed_train(water, [&](std::uint32_t, const GerstnerContributor& contributor,
                                     const WaterWave& wave) {
        WaveTerms t = terms_for(wave, contributor, world_xz, time);
        if (t.amplitude == 0.0f) return;

        float c = std::cos(t.phase);
        float s = std::sin(t.phase);
        float wa = t.k * t.amplitude;

        binormal.x -= t.q * t.dir.x * t.dir.x * wa * s;
        binormal.y += t.dir.x * wa * c;
        binormal.z -= t.q * t.dir.x * t.dir.y * wa * s;

        tangent.x -= t.q * t.dir.x * t.dir.y * wa * s;
        tangent.y += t.dir.y * wa * c;
        tangent.z -= t.q * t.dir.y * t.dir.y * wa * s;
    });
    if (water.simulated.has_value()) {
        // Tangents add across contributors where normals do not, so the
        // simulated waves go in before the cross product, as in the shader.
        const SpectralDerivatives d = spectral_derivatives(
            water.simulated->waves, water.simulated->choppiness, world_xz, time);
        binormal += glm::vec3(d.xx, d.dh_dx, d.xz);
        tangent += glm::vec3(d.xz, d.dh_dz, d.zz);
    }

    glm::vec3 n = glm::cross(tangent, binormal);
    float len = glm::length(n);
    if (len < 1e-6f) return glm::vec3(0.0f, 1.0f, 0.0f);
    return n / len;
}

float horizontal_jacobian(const FjWaterData& water, glm::vec2 world_xz, float time) {
    // 1 at rest, 0 where the surface has pinched to a point and negative where
    // it has folded.
    return jacobian_terms(water, world_xz, time).determinant();
}

float folded_share(const FjWaterData& water) {
    // Pinched to a point counts as folded, which is where crest() reads 1.
    constexpr float FOLDED_AT = 1e-3f;

    const FftContributor* simulated = simulated_sea(water);
    if (simulated == nullptr || simulated->resolution < 2 || simulated->patch_size <= 0.0f) {
        // Closed-form only: sample it across a stretch of sea and a few
        // moments, since a Gerstner sea's folds come and go as trains align.
        int folded = 0;
        int total = 0;
        for (float x = -120.0f; x <= 120.0f; x += 6.0f) {
            for (float z = -120.0f; z <= 120.0f; z += 6.0f) {
                for (float time : {0.0f, 5.5f, 13.7f}) {
                    ++total;
                    if (jacobian_terms(water, {x, z}, time).determinant() <= FOLDED_AT) ++folded;
                }
            }
        }
        return static_cast<float>(folded) / static_cast<float>(total);
    }

    // With a simulated sea, measure on its own grid: every sample of its patch
    // at one moment. A spectrum is the same sea statistically at any time, and
    // sixty-five thousand samples of it are more than the closed-form case
    // takes across its three.
    constexpr float TIME = 0.0f;
    const SpectrumField field = generate_spectrum(*simulated);
    const JacobianFields simulated_part = jacobian_fields(field, simulated->choppiness, TIME);
    const std::uint32_t n = field.resolution;
    const float texel = simulated->patch_size / static_cast<float>(n);

    std::size_t folded = 0;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::size_t cell = static_cast<std::size_t>(z) * n + x;
            // The Gerstner share only: the simulated sea is measured whole
            // just above, and a field that also carries its strongest waves
            // for physics would otherwise have them counted twice.
            HorizontalJacobian j = gerstner_jacobian_terms(
                water, {static_cast<float>(x) * texel, static_cast<float>(z) * texel}, TIME);
            j.xx += simulated_part.xx[cell];
            j.zz += simulated_part.zz[cell];
            j.xz += simulated_part.xz[cell];
            if (j.determinant() <= FOLDED_AT) ++folded;
        }
    }
    return static_cast<float>(folded) / static_cast<float>(static_cast<std::size_t>(n) * n);
}

float crest(const FjWaterData& water, glm::vec2 world_xz, float time) {
    // Mapped to 0..1 rising into the crest so shaders can use it directly as a
    // foam mask. The clamp means a folded surface reads the same as one
    // pinched exactly to a point; horizontal_jacobian() keeps the difference.
    return std::clamp(1.0f - horizontal_jacobian(water, world_xz, time), 0.0f, 1.0f);
}

glm::vec2 dominant_direction(const FjWaterData& water) {
    glm::vec2 sum(0.0f);
    for_each_summed_train(water, [&](std::uint32_t, const GerstnerContributor& contributor,
                                     const WaterWave& w) {
        sum += (w.direction / glm::length(w.direction))
             * (w.amplitude * contributor.amplitude_scale);
    });
    if (const auto* fft = simulated_sea(water); fft != nullptr) {
        // A simulated sea runs downwind, and votes with the amplitude it has.
        const float wind_len = glm::length(fft->wind_direction);
        if (wind_len >= 1e-6f) {
            sum += (fft->wind_direction / wind_len) * significant_amplitude(*fft);
        }
    }
    float len = glm::length(sum);
    return len < 1e-6f ? glm::vec2(0.0f) : sum / len;
}

float wave_height(const FjWaterData& water) {
    float sum = 0.0f;
    for_each_summed_train(water, [&](std::uint32_t, const GerstnerContributor& contributor,
                                     const WaterWave& wave) {
        sum += wave.amplitude * contributor.amplitude_scale;
    });
    if (const auto* fft = simulated_sea(water)) sum += significant_amplitude(*fft);
    return sum;
}

float shortest_wavelength(const FjWaterData& water) {
    float shortest = 0.0f;
    for_each_summed_train(water, [&](std::uint32_t, const GerstnerContributor&,
                                     const WaterWave& wave) {
        shortest = shortest > 0.0f ? std::min(shortest, wave.wavelength) : wave.wavelength;
    });
    if (const auto* fft = simulated_sea(water); fft != nullptr && fft->resolution >= 2) {
        // Two samples to a wave is the shortest a grid can hold.
        const float finest = std::max(2.0f * fft->patch_size / static_cast<float>(fft->resolution),
                                      fft->high_cut);
        shortest = shortest > 0.0f ? std::min(shortest, finest) : finest;
    }
    return shortest;
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
        const HorizontalJacobian j = jacobian_terms(water, rest, time);
        const float det = j.determinant();
        // A vanishing determinant is the surface pinched to a point, where
        // there is no single rest position to solve for. Stop rather than
        // divide through it; the current guess is as good as it gets.
        if (std::abs(det) < 1e-6f) break;

        rest -= glm::vec2(( j.zz * error.x - j.xz * error.y) / det,
                          (-j.xz * error.x + j.xx * error.y) / det);
    }
    return rest;
}

float height_at_iterative(const FjWaterData& water, glm::vec2 world_xz, float time) {
    return displace(water, rest_position_for(water, world_xz, time), time).y;
}

} // namespace fjell::water
