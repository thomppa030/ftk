#pragma once

#include <glm/glm.hpp>

#include <memory>
#include <optional>

namespace fjell {

struct FjTerrainData;

namespace water {

/// The ground under a body of water, as the wave field reads it: a terrain's
/// heights, where that terrain stands and where the water stands at rest.
///
/// Waves answer to how deep the water is under them — they grow, shorten and
/// break as it shallows — and a field that does not know the ground puts full
/// waves on dry sand. The bed is the terrain's own CPU heights, shared rather
/// than copied: the same array physics collides with, so a wave dies where a
/// character's feet find the ground.
///
/// A terrain places its field by translation alone (see TerrainAsset), so
/// the whole placement is a corner and a height.
struct ShoreBed {
    std::shared_ptr<const FjTerrainData> terrain;
    /// World XZ of the terrain's min corner, where its field coordinates start.
    glm::vec2 min_corner{0.0f};
    /// World Y of the terrain node, which its heights stand on.
    float ground_y{0.0f};
    /// World Y of the water at rest.
    float surface_y{0.0f};
};

/// How deep the still water is at a point, and how that changes across it.
struct StillDepth {
    /// Metres from the surface at rest down to the bed; negative where the
    /// bed stands above the water.
    float depth{0.0f};
    /// d(depth)/dx and d(depth)/dz. The bed is read bilinearly between its
    /// samples, so this is the slope of that surface, constant along a cell
    /// in one direction and stepping at the sample lines in the other.
    glm::vec2 gradient{0.0f};
};

/// Where deep water begins, as the depth in units of a wave's own length:
/// `μ = k · d`, wavenumber times depth. Past it a wave cannot feel the bed —
/// the shoaling coefficient is within 1e-5 of 1 — and the shore leaves it
/// exactly as it is. About 1.1 wavelengths of water.
inline constexpr float SHOALING_DEEP_MU = 7.0f;

/// Below this `μ` the coefficient is held where it is. It rises without bound
/// as the depth goes to nothing, but by then breaking has held the wave far
/// below any height it could reach: a 40 m wave is 6 mm from the waterline.
inline constexpr float SHOALING_SHALLOWEST_MU = 1e-3f;

/// A wave breaks when it stands taller, crest to trough, than this share of
/// the water under it (McCowan's 0.78). Halved, it is the amplitude the
/// shallows can hold.
inline constexpr float BREAKER_INDEX = 0.78f;

/// How much a wave has grown for having shoaled: the shoaling coefficient
/// `Ks`, and how it changes with `μ`.
struct Shoaling {
    float coefficient{1.0f};
    float slope{0.0f};   // dKs/dμ
};

/// Green's law in its exact form: the amplitude a wave has at depth `μ = k·d`
/// against the amplitude it had in deep water, from its energy flux staying
/// the same as it slows. Dips to 0.91 around `μ = 1` and then climbs: 1.28 at
/// `μ = 0.1`, 2.24 at 0.01. Exactly 1 from SHOALING_DEEP_MU on.
///
/// A fitted series rather than the physics solved each time: the physics wants
/// a `tanh` and a `sinh`, which the C libraries do not round alike, and a
/// point of the sea has to come out the same on every machine. Within 1e-4 of
/// the solved value across its range.
[[nodiscard]] Shoaling shoaling_at(float mu);

/// What one sum of the sea shoals by: its dominant wave's wavenumber and the
/// sum's significant amplitude — half its significant wave height, the height
/// breaking limits.
struct ShoalingSum {
    float k{0.0f};           // rad/m
    float amplitude{0.0f};   // metres
};

/// What the ground does to the sea at one point: a factor on each sum, how
/// that factor changes across the point, and how hard the water is breaking.
///
/// Both sums of the sea — the Gerstner trains and the simulated sea — grow by
/// their own dominant wave's coefficient, and breaking then holds them to what
/// the depth can carry. Breaking is a limit on how tall the water is, so it
/// takes the two together: their shoaled heights add as variances, and one
/// ratio scales both.
struct ShoreEffect {
    float gerstner{1.0f};
    glm::vec2 gerstner_gradient{0.0f};
    float simulated{1.0f};
    glm::vec2 simulated_gradient{0.0f};
    /// The share of its shoaled height the water has lost to breaking: 0 where
    /// the depth carries all of it, rising to 1 at the waterline and on dry
    /// ground.
    float breaking{0.0f};
};

/// The shore's effect on a sea made of `gerstner` and `simulated` over water
/// `still` deep, or nothing where it has none: where every sum present is in
/// deep water, and on a flat sea. Nothing means the sea is left exactly as
/// it is, not scaled by 1.
[[nodiscard]] std::optional<ShoreEffect> shore_effect(const StillDepth& still,
                                                      ShoalingSum gerstner,
                                                      ShoalingSum simulated);

/// The still-water depth at world `world_xz`, or nothing where the bed is not
/// known: off the terrain, or with no terrain at all. Nothing is open water —
/// no bed near enough to matter — and not a depth of zero.
///
/// Read bilinearly between samples, the way the terrain draws itself, so the
/// waterline is where the drawn ground meets the water.
[[nodiscard]] std::optional<StillDepth> still_depth(const ShoreBed& bed, glm::vec2 world_xz);

} // namespace water
} // namespace fjell
