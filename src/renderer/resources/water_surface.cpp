#include "renderer/resources/water_surface.hpp"

#include "renderer/resources/water_spectrum.hpp"

#include "core/simd_trig.hpp"

#include <algorithm>
#include <cassert>
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

// One train as the sum wants it. Only ever handed a train that moves the
// surface, which is what lets it divide by the direction's length and the
// wavelength without asking.
struct PreparedTrain {
    glm::vec2 dir;
    float k;
    float amplitude;
    float q;
    float omega;
};

[[nodiscard]] PreparedTrain prepare(const WaterWave& wave, const GerstnerContributor& contributor) {
    PreparedTrain t;
    t.dir = wave.direction / glm::length(wave.direction);
    t.k = TAU / wave.wavelength;
    t.amplitude = wave.amplitude * contributor.amplitude_scale;
    t.q = steepness_to_q(wave.steepness, t.k, t.amplitude,
                         steepness_budget(contributor.steepness_headroom));
    t.omega = std::sqrt(GRAVITY * t.k) * wave.speed * contributor.speed_scale;
    return t;
}

// Eight trains' phases at a point and time, and their sines and cosines.
struct TrainAngles {
    simd::f32x8 sin;
    simd::f32x8 cos;
};

[[nodiscard]] TrainAngles train_angles(const PackedTrains& t, std::size_t i, glm::vec2 world_xz,
                                       float time) {
    // dot(dir, xz) * k - omega * t.
    const simd::f32x8 dx = _mm256_loadu_ps(&t.dir_x[i]);
    const simd::f32x8 dz = _mm256_loadu_ps(&t.dir_z[i]);
    simd::f32x8 along = _mm256_fmadd_ps(dz, _mm256_set1_ps(world_xz.y),
                                        _mm256_mul_ps(dx, _mm256_set1_ps(world_xz.x)));
    simd::f32x8 phase = _mm256_fnmadd_ps(_mm256_loadu_ps(&t.omega[i]), _mm256_set1_ps(time),
                                         _mm256_mul_ps(along, _mm256_loadu_ps(&t.k[i])));
    TrainAngles a;
    simd::sincos(phase, a.sin, a.cos);
    return a;
}

[[nodiscard]] float sum8(simd::f32x8 v) {
    const __m128 lo = _mm256_castps256_ps128(v);
    const __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 r = _mm_add_ps(lo, hi);
    r = _mm_add_ps(r, _mm_movehl_ps(r, r));
    r = _mm_add_ss(r, _mm_shuffle_ps(r, r, 1));
    return _mm_cvtss_f32(r);
}

// The trains the evaluators sum. A field reaches them prepared — through
// apply_wind(), which is how every field the world runs on is made — or not
// at all: preparing here instead would be six allocations per sample point,
// and a fallback that costs three times the fast path is a trap, not a
// convenience.
[[nodiscard]] const PackedTrains& trains_of(const FjWaterData& water) {
    assert(water.packed_trains.has_value()
           && "evaluating a field whose trains were never prepared: derive it with "
              "water::apply_wind() first");
    return *water.packed_trains;
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
    const PackedTrains& t = trains_of(water);
    simd::f32x8 xx = _mm256_setzero_ps(), zz = _mm256_setzero_ps(), xz = _mm256_setzero_ps();
    for (std::size_t i = 0; i < t.k.size(); i += 8) {
        const TrainAngles a = train_angles(t, i, world_xz, time);
        const simd::f32x8 dx = _mm256_loadu_ps(&t.dir_x[i]);
        const simd::f32x8 dz = _mm256_loadu_ps(&t.dir_z[i]);
        // wa = k * A * q, times sin.
        const simd::f32x8 was = _mm256_mul_ps(
            _mm256_mul_ps(_mm256_mul_ps(_mm256_loadu_ps(&t.k[i]), _mm256_loadu_ps(&t.amplitude[i])),
                          _mm256_loadu_ps(&t.q[i])),
            a.sin);
        xx = _mm256_fnmadd_ps(_mm256_mul_ps(dx, dx), was, xx);
        zz = _mm256_fnmadd_ps(_mm256_mul_ps(dz, dz), was, zz);
        xz = _mm256_fnmadd_ps(_mm256_mul_ps(dx, dz), was, xz);
    }
    HorizontalJacobian j;
    j.xx += sum8(xx);
    j.zz += sum8(zz);
    j.xz += sum8(xz);
    return j;
}

// Everything the CPU sums: the Gerstner contributors, and the simulated sea's
// strongest waves where the field has been given them.
[[nodiscard]] HorizontalJacobian jacobian_terms(const FjWaterData& water, glm::vec2 world_xz,
                                                float time) {
    HorizontalJacobian j = gerstner_jacobian_terms(water, world_xz, time);
    if (water.simulated.has_value()) {
        const SpectralDerivatives d = spectral_derivatives(
            water.simulated->packed, water.simulated->choppiness, world_xz, time);
        j.xx += d.xx;
        j.zz += d.zz;
        j.xz += d.xz;
    }
    return j;
}

} // namespace

PackedTrains pack_trains(const FjWaterData& water) {
    std::vector<PreparedTrain> prepared;
    for_each_summed_train(water, [&](std::uint32_t, const GerstnerContributor& contributor,
                                     const WaterWave& wave) {
        prepared.push_back(prepare(wave, contributor));
    });
    PackedTrains p;
    p.count = prepared.size();
    const std::size_t padded = (prepared.size() + 7) / 8 * 8;
    for (auto* field : {&p.dir_x, &p.dir_z, &p.k, &p.amplitude, &p.q, &p.omega}) {
        field->assign(padded, 0.0f);
    }
    // A padding train has amplitude zero and adds nothing whatever its phase.
    for (std::size_t i = 0; i < prepared.size(); ++i) {
        p.dir_x[i] = prepared[i].dir.x;
        p.dir_z[i] = prepared[i].dir.y;
        p.k[i] = prepared[i].k;
        p.amplitude[i] = prepared[i].amplitude;
        p.q[i] = prepared[i].q;
        p.omega[i] = prepared[i].omega;
    }
    return p;
}

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
        sea.packed = pack_waves(sea.waves);
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
    out.packed_trains = pack_trains(out);
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
    const PackedTrains& t = trains_of(water);
    simd::f32x8 dx = _mm256_setzero_ps(), dy = _mm256_setzero_ps(), dz = _mm256_setzero_ps();
    simd::f32x8 vx = _mm256_setzero_ps(), vy = _mm256_setzero_ps(), vz = _mm256_setzero_ps();
    for (std::size_t i = 0; i < t.k.size(); i += 8) {
        const TrainAngles a = train_angles(t, i, world_xz, time);
        const simd::f32x8 amp = _mm256_loadu_ps(&t.amplitude[i]);
        const simd::f32x8 omega = _mm256_loadu_ps(&t.omega[i]);
        // q * A * dir, the horizontal bunching toward the crest; then the crest.
        const simd::f32x8 qa = _mm256_mul_ps(_mm256_loadu_ps(&t.q[i]), amp);
        const simd::f32x8 qax = _mm256_mul_ps(qa, _mm256_loadu_ps(&t.dir_x[i]));
        const simd::f32x8 qaz = _mm256_mul_ps(qa, _mm256_loadu_ps(&t.dir_z[i]));
        dx = _mm256_fmadd_ps(qax, a.cos, dx);
        dz = _mm256_fmadd_ps(qaz, a.cos, dz);
        dy = _mm256_fmadd_ps(amp, a.sin, dy);
        // The phase falls at omega: cos becomes omega sin, sin becomes -omega cos.
        vx = _mm256_fmadd_ps(_mm256_mul_ps(qax, a.sin), omega, vx);
        vz = _mm256_fmadd_ps(_mm256_mul_ps(qaz, a.sin), omega, vz);
        vy = _mm256_fnmadd_ps(_mm256_mul_ps(amp, a.cos), omega, vy);
    }
    m.displacement = {sum8(dx), sum8(dy), sum8(dz)};
    m.velocity = {sum8(vx), sum8(vy), sum8(vz)};
    if (water.simulated.has_value()) {
        const SpectralMotion s = spectral_motion(water.simulated->packed,
                                                 water.simulated->choppiness, world_xz, time);
        m.displacement += s.displacement;
        m.velocity += s.velocity;
    }
    return m;
}

glm::vec3 displace(const FjWaterData& water, glm::vec2 world_xz, float time) {
    return surface_motion(water, world_xz, time).displacement;
}

glm::vec3 normal_at(const FjWaterData& water, glm::vec2 world_xz, float time) {
    // Partial derivatives of the displaced surface with respect to the rest
    // position. The normal is the cross product of the two tangents; writing
    // the sums out avoids building both tangents separately.
    const PackedTrains& t = trains_of(water);
    simd::f32x8 bx = _mm256_setzero_ps(), by = _mm256_setzero_ps(), bz = _mm256_setzero_ps();
    simd::f32x8 tx = _mm256_setzero_ps(), ty = _mm256_setzero_ps(), tz = _mm256_setzero_ps();
    for (std::size_t i = 0; i < t.k.size(); i += 8) {
        const TrainAngles a = train_angles(t, i, world_xz, time);
        const simd::f32x8 dx = _mm256_loadu_ps(&t.dir_x[i]);
        const simd::f32x8 dz = _mm256_loadu_ps(&t.dir_z[i]);
        const simd::f32x8 wa = _mm256_mul_ps(_mm256_loadu_ps(&t.k[i]), _mm256_loadu_ps(&t.amplitude[i]));
        const simd::f32x8 qwas = _mm256_mul_ps(_mm256_mul_ps(_mm256_loadu_ps(&t.q[i]), wa), a.sin);
        const simd::f32x8 wac = _mm256_mul_ps(wa, a.cos);
        bx = _mm256_fnmadd_ps(_mm256_mul_ps(dx, dx), qwas, bx);
        by = _mm256_fmadd_ps(dx, wac, by);
        bz = _mm256_fnmadd_ps(_mm256_mul_ps(dx, dz), qwas, bz);
        tx = _mm256_fnmadd_ps(_mm256_mul_ps(dx, dz), qwas, tx);
        ty = _mm256_fmadd_ps(dz, wac, ty);
        tz = _mm256_fnmadd_ps(_mm256_mul_ps(dz, dz), qwas, tz);
    }
    glm::vec3 binormal{1.0f + sum8(bx), sum8(by), sum8(bz)};  // d/dx
    glm::vec3 tangent{sum8(tx), sum8(ty), 1.0f + sum8(tz)};   // d/dz
    if (water.simulated.has_value()) {
        // Tangents add across contributors where normals do not, so the
        // simulated waves go in before the cross product, as in the shader.
        const SpectralDerivatives d = spectral_derivatives(
            water.simulated->packed, water.simulated->choppiness, world_xz, time);
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

float folded_share(const FjWaterData& authored) {
    // Pinched to a point counts as folded, which is where crest() reads 1.
    constexpr float FOLDED_AT = 1e-3f;

    // Asked about an authored field, once per edit; the evaluators want its
    // trains prepared, which is a moment's work against the sampling below.
    FjWaterData water = authored;
    water.packed_trains = pack_trains(water);

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
