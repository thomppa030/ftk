#pragma once

#include "renderer/resources/fjwater.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <variant>

namespace fjell {

/// Evaluates a `.fjwater` wave field on the CPU.
///
/// The shader evaluates the same field with the same formula in
/// shaders/include/fjell_water_waves.glsl. Every function here has a
/// counterpart there, written to match line for line, because the whole point
/// of defining waves in an asset rather than in GLSL is that physics can ask
/// where the surface is and get the answer the player is looking at. A boat
/// floating inside a wave is what drift between the two looks like, which is
/// why test_fjwater.cpp compiles the GLSL and compares the two at a grid of
/// sample points rather than trusting that they were kept in step.
///
/// Gerstner displaces horizontally as well as vertically, so a vertex does not
/// stay above its rest position. displace() gives the full offset; height_at()
/// is the vertical part only and is an approximation of the true surface
/// height at a world XZ (see its comment).
namespace water {

/// Whether a train can move the surface at all. One with no direction to
/// travel in, or no length, cannot.
[[nodiscard]] inline bool moves_surface(const WaterWave& wave) {
    return glm::length(wave.direction) >= 1e-6f && wave.wavelength >= 1e-4f;
}

/// Calls `fn(slot, contributor, wave)` for every train the surface sums.
///
/// That is the first MAX_WATER_WAVES trains that move the surface, of each of
/// the first MAX_GERSTNER_CONTRIBUTORS Gerstner contributors; `slot` counts
/// those contributors from 0. This is the only place that is decided. The CPU
/// evaluator, every query over the field and the packing for the shader all
/// walk the field through here, so the sea physics floats a boat on and the
/// sea that is drawn are made of the same trains by construction rather than
/// by two rules kept in step.
template <typename Fn>
void for_each_summed_train(const FjWaterData& water, Fn&& fn) {
    std::uint32_t slot = 0;
    for (const auto& contributor : water.contributors) {
        const auto* gerstner = std::get_if<GerstnerContributor>(&contributor);
        if (gerstner == nullptr) continue;
        if (slot >= MAX_GERSTNER_CONTRIBUTORS) break;

        std::uint32_t summed = 0;
        for (const auto& wave : gerstner->waves) {
            if (summed >= MAX_WATER_WAVES) break;
            if (!moves_surface(wave)) continue;
            fn(slot, *gerstner, wave);
            ++summed;
        }
        ++slot;
    }
}

/// Deep-water angular frequency for a wavelength: waves of different lengths
/// travel at different speeds, which is what keeps a sum of trains from
/// looking like one rigid sheet. omega = sqrt(g * k).
[[nodiscard]] float wave_frequency(float wavelength);

/// A Gerstner contributor as the wind has made it: heights raised or
/// dropped, crests sharpened, and the short trains turned onto the wind while
/// the swell holds its heading. The turn ramps by wavelength between the
/// longest and shortest train of this contributor.
///
/// Returns its input unchanged when its response is disabled or the wind is
/// its reference wind.
///
/// `wind_xz` is the wind on the ground plane in m/s, direction and magnitude
/// together; the caller has already folded in the gust.
[[nodiscard]] GerstnerContributor apply_wind(const GerstnerContributor& water,
                                             glm::vec2 wind_xz);

/// The authored field as the wind has made it: every contributor's own answer
/// to the same wind.
///
/// Each kind of contributor answers in its own way and by its own response,
/// so one sea can hold a swell that rolls through unmoved and a chop that
/// turns with the weather.
///
/// A derivation rather than an edit. The `.fjwater` on disk stays the sea the
/// artist tuned, and this produces what the world actually has right now, so
/// the same asset can be a millpond and a gale without being two files.
///
/// Everything downstream — both evaluators, the GPU packing, buoyancy — reads
/// the result of this rather than the authored field, which is the only way
/// the surface a boat floats on stays the surface the player sees.
[[nodiscard]] FjWaterData apply_wind(const FjWaterData& water, glm::vec2 wind_xz);

/// How much of a rise or a fall in wave height a sea takes on per second,
/// as the seconds a change takes to mostly arrive.
///
/// Building is slower than dying because that is the way round it happens: a
/// wind raises a sea over hours, and when the wind drops the swell it made
/// keeps rolling for longer still. These are seconds rather than hours
/// because a storm in a game passes in minutes, and a sea answering on the
/// real timescale would never visibly answer at all.
inline constexpr float SEA_RISE_SECONDS = 25.0f;
inline constexpr float SEA_FALL_SECONDS = 45.0f;

/// One step of the sea catching up to the wind that is blowing.
///
/// Water has mass, and a wave field driven straight from the weather does not
/// show it: a weather event stepping its wind at a phase boundary would step
/// the whole sea with it, and the gust — which breathes about once a second —
/// would heave the entire ocean up and down. This lags the wind the field is
/// built from so a front raises a sea over its passage rather than at its
/// arrival.
///
/// Frame-rate independent: the same curve at any `dt`.
[[nodiscard]] glm::vec2 advance_sea_wind(glm::vec2 sea_wind, glm::vec2 blowing, float dt);

/// How long suspended sediment takes to stir up, and to settle back out.
///
/// Far more lopsided than the sea's own rise and fall, because the two
/// processes are not each other's opposite: waves lift sediment off the bed
/// in the minutes a squall takes to arrive, and then it falls back out under
/// gravity over hours. Water going clear the moment a gust passes is the same
/// error as a gust heaving the ocean — the sea state is the cause, and the
/// cloud it leaves outlives it.
///
/// Compressed from the real timescale for the same reason the sea's rates
/// are: a storm in a game passes in minutes.
inline constexpr float SILT_STIR_SECONDS = 20.0f;
inline constexpr float SILT_SETTLE_SECONDS = 240.0f;

/// How strongly this sea stirs the bed under `depth` metres of water, 0 (no
/// reach) to 1 (working the bed hard).
///
/// Waves stir sediment by the orbital motion of the water under them, and
/// that motion dies off exponentially with depth — as exp(-k·depth) for
/// wavenumber k. So this is a question about the bed rather than the surface,
/// and it is answered per train: wavelength decides the reach, not height. A
/// five-metre chop is spent by five metres down while a long swell of the
/// same height still works a thirty-metre bed, which is why a storm clouds a
/// coast and leaves an ocean clear.
///
/// `depth` is the still-water depth at the point asked about. Returns 0 for
/// water deeper than the trains can reach, and for a flat sea at any depth.
///
/// The CPU has no depth map — how deep the water is at a pixel comes from the
/// depth buffer, in the shader — so this answers for one depth at a time. The
/// renderer asks at a reference depth to drive the lag, and the shader scales
/// what it gets by the depth it can actually see. A CPU client that does know
/// its depth, buoyancy or a gameplay query, can ask directly.
[[nodiscard]] float bed_agitation(const FjWaterData& water, float depth);

/// The depth at which the sea's stirring is quoted to the shader.
///
/// The load a material reads is the load over a bed this far down; the shader
/// reweights it for the depth each pixel actually has. Shallow enough that a
/// coast is the thing being described, deep enough that it is not saturated
/// by every passing ripple.
inline constexpr float SILT_REFERENCE_DEPTH = 3.0f;

/// One step of suspended sediment answering the sea working the bed.
///
/// `load` is what is currently in suspension, 0 clear to 1 as thick as this
/// water gets; `agitation` what the sea is doing to the bed right now, from
/// `bed_agitation()`. Rises toward the agitation and falls toward nothing,
/// on the two rates above.
///
/// Frame-rate independent: the same curve at any `dt`.
[[nodiscard]] float advance_silt(float load, float agitation, float dt);

/// Where the surface at rest position `world_xz` has moved to at `time`.
/// The x and z components are the horizontal bunching toward crests, y the
/// height. Add this to a flat vertex to get the water surface.
[[nodiscard]] glm::vec3 displace(const FjWaterData& water, glm::vec2 world_xz, float time);

/// Surface normal at the same rest position, from the analytic derivatives of
/// displace(). Cheaper and smoother than finite differences and exact for the
/// field as defined.
[[nodiscard]] glm::vec3 normal_at(const FjWaterData& water, glm::vec2 world_xz, float time);

/// The Jacobian of the horizontal displacement: 1 on flat water, falling as
/// the trains bunch together, 0 where the surface has pinched to a point and
/// **negative where it has folded through itself**.
///
/// crest() is this mapped into 0..1 for shaders, and the clamp it does throws
/// away the sign — which is the one thing a fold check needs. Anything asking
/// whether a field folds, and by how much, reads this instead.
[[nodiscard]] float horizontal_jacobian(const FjWaterData& water, glm::vec2 world_xz,
                                        float time);

/// How pinched the surface is here, 0 on flat water rising toward 1 at a
/// crest about to fold. This is horizontal_jacobian() mapped for shaders to
/// use directly as a foam mask: where it approaches 1 the surface has bunched
/// to a point, which is exactly where whitecaps form, so foam reads off the
/// wave field rather than being painted separately.
///
/// Clamped, so a folded surface and one pinched exactly to a point both read
/// as 1. Use horizontal_jacobian() to tell them apart.
[[nodiscard]] float crest(const FjWaterData& water, glm::vec2 world_xz, float time);

/// The way this sea travels, as the amplitude-weighted mean of its trains,
/// or (0,0) when they cancel.
///
/// A property of the field rather than of a point, so it is worked out here,
/// once, and the pass hands the answer to the shaders as a push constant rather
/// than having them work it out again per vertex. Water materials read it as
/// WAVE_DIR, which is what lets foam gather on the face of
/// a wave that is advancing rather than sitting evenly along the crest.
///
/// Weighted by amplitude because the train carrying the surface is the one
/// that sets the sea's direction; a ripple a hundredth its height does not get
/// an equal vote. That is the amplitude a train actually has, under its
/// contributor's scale, so a contributor turned down to nothing stops steering
/// the sea as well as stops raising it. Trains pointing opposite ways cancel, and a sea with two
/// equal opposed swells genuinely has no direction of travel — the zero says
/// so, and callers should fall back to something non-directional.
[[nodiscard]] glm::vec2 dominant_direction(const FjWaterData& water);

/// How big this sea has got, in metres: the sum of every summed train's
/// amplitude, each under its own contributor's scale. Contributors add into
/// one surface, so this is the highest that surface can stand above rest with
/// every crest of every contributor aligned. 0 for flat water.
///
/// A property of the field rather than of a point. It is what scripts read as
/// `Water::wave_height()`, so what it means is part of the script ABI.
[[nodiscard]] float wave_height(const FjWaterData& water);

/// The shortest wavelength among the trains the surface sums, in metres, or
/// 0 when there are none. The train a tessellation has the hardest time drawing.
[[nodiscard]] float shortest_wavelength(const FjWaterData& water);

/// Height of the surface above `world_xz`, for buoyancy and swimming.
///
/// Note this evaluates the field at `world_xz` as a rest position, so under
/// steep waves the returned height belongs to a point that has drifted
/// slightly horizontally. For floating bodies that error is far below the
/// wave height and costs nothing; iterate with height_at_iterative() if a
/// use ever needs the true vertical column.
[[nodiscard]] float height_at(const FjWaterData& water, glm::vec2 world_xz, float time);

/// The rest position whose displacement lands on `world_xz` — the point of
/// the flat surface that the waves have carried over that column.
///
/// Solved by Newton over the horizontal displacement, to a millimetre or at
/// most eight steps. Flat and moderate water converges in two or three; the
/// budget is there for steep seas, where the first steps can overshoot before
/// settling. On a steep sea the surface can also stand over the same column in
/// more than one place, and this returns whichever branch the solve reaches —
/// every one of them is the real surface there.
///
/// Callers that want several quantities at one spot should solve once and
/// pass the result to displace()/normal_at()/crest(), rather than asking each
/// of them to solve again.
[[nodiscard]] glm::vec2 rest_position_for(const FjWaterData& water, glm::vec2 world_xz,
                                          float time);

/// height_at() refined through rest_position_for(), so the answer is the
/// height of the water actually above that column rather than of the water
/// that started there. For when a body must sit on the surface it is touching.
[[nodiscard]] float height_at_iterative(const FjWaterData& water, glm::vec2 world_xz, float time);

} // namespace water
} // namespace fjell
