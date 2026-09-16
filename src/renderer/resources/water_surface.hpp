#pragma once

#include "renderer/resources/fjwater.hpp"

#include <glm/glm.hpp>

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

/// Deep-water angular frequency for a wavelength: waves of different lengths
/// travel at different speeds, which is what keeps a sum of trains from
/// looking like one rigid sheet. omega = sqrt(g * k).
[[nodiscard]] float wave_frequency(float wavelength);

/// Where the surface at rest position `world_xz` has moved to at `time`.
/// The x and z components are the horizontal bunching toward crests, y the
/// height. Add this to a flat vertex to get the water surface.
[[nodiscard]] glm::vec3 displace(const FjWaterData& water, glm::vec2 world_xz, float time);

/// Surface normal at the same rest position, from the analytic derivatives of
/// displace(). Cheaper and smoother than finite differences and exact for the
/// field as defined.
[[nodiscard]] glm::vec3 normal_at(const FjWaterData& water, glm::vec2 world_xz, float time);

/// How pinched the surface is here, 0 on flat water rising toward 1 at a
/// crest about to fold. This is the Jacobian of the horizontal displacement:
/// where it approaches zero the surface has bunched to a point, which is
/// exactly where whitecaps form, so foam reads off the wave field rather than
/// being painted separately.
[[nodiscard]] float crest(const FjWaterData& water, glm::vec2 world_xz, float time);

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
