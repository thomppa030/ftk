#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace fjell {

struct FjTerrainData;

namespace water {

/// How much of each sum of the sea reaches the water around a terrain past its
/// land: the share of the sum's amplitude left at each point, 0 where land
/// stands between the point and every way the sum arrives, 1 where nothing
/// does. Baked from the terrain by `bake_exposure()` (water_shelter.hpp) and
/// read bilinearly between its samples, like the bed.
///
/// The grid reaches past the terrain by a margin over which it fades back to
/// 1, so the lee of land near the terrain's edge does not end in a line where
/// the open sea begins.
struct ShoreExposure {
    std::uint32_t samples{0};      // per side
    glm::vec2 min_corner{0.0f};    // world XZ of sample (0, 0)
    float size{0.0f};              // metres from the first sample to the last
    /// Per sample, row-major as `[z * samples + x]`: the Gerstner trains'
    /// share and the simulated sea's, interleaved, as 16-bit fractions of 1.
    /// The shader's texture holds these same numbers, so both halves read one
    /// value rather than two roundings of it.
    std::vector<std::uint16_t> shares;
};

/// Where each sum of the sea is read near a terrain: the sum at a point `p`
/// is its deep-water self at `p + offset`. Waves that slow in shallow water
/// fall behind the deep sea they came from, and reading the deep sea at a
/// point it has not yet reached is that lag: crests closer together and
/// slower over the shallows, and frequency, which water conserves, untouched.
/// Baked from the terrain by `bake_warp()` (water_shelter.hpp) and read
/// bilinearly between its samples.
///
/// On a coarser grid than the exposure, since a running sum is smooth, over
/// the same ground; it fades back to no offset across the same margin.
struct ShoreWarp {
    std::uint32_t samples{0};      // per side
    glm::vec2 min_corner{0.0f};    // world XZ of sample (0, 0)
    float size{0.0f};              // metres from the first sample to the last
    /// Per sample, row-major as `[z * samples + x]`: the Gerstner trains'
    /// offset (x, z) and the simulated sea's (x, z), in metres. The shader's
    /// texture holds these same floats.
    std::vector<float> offsets;
};

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
    /// The terrain's edit generation the heights are at, which says when the
    /// land has changed under the exposure.
    std::uint32_t generation{0};
    /// How open the water is to the sea, or nothing: fully open everywhere.
    std::shared_ptr<const ShoreExposure> exposure;
    /// Where each sum of the sea is read, or nothing: at the point itself.
    std::shared_ptr<const ShoreWarp> warp;
};

/// Where one sum of the sea is read at a point, and how that moves across
/// it: `jacobian` is d(p + offset)/dp, the identity where nothing is warped.
struct SeaWarp {
    glm::vec2 offset{0.0f};
    glm::mat2 jacobian{1.0f};

    [[nodiscard]] bool identity() const {
        return offset == glm::vec2(0.0f) && jacobian == glm::mat2(1.0f);
    }
};

/// Both sums' warps at one point.
struct WarpAt {
    SeaWarp trains;
    SeaWarp simulated;

    [[nodiscard]] bool identity() const { return trains.identity() && simulated.identity(); }
};

/// The warp at world `world_xz`, read bilinearly between its samples; none off
/// its grid.
[[nodiscard]] WarpAt warp_at(const ShoreWarp& warp, glm::vec2 world_xz);

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

/// How open one point is to each sum of the sea — its amplitude share from a
/// ShoreExposure — and how that changes across the point.
struct ExposureAt {
    float trains{1.0f};
    float simulated{1.0f};
    glm::vec2 trains_gradient{0.0f};
    glm::vec2 simulated_gradient{0.0f};

    [[nodiscard]] bool open() const { return trains >= 1.0f && simulated >= 1.0f; }
};

/// The exposure at world `world_xz`, read bilinearly between its samples;
/// wholly open off its grid.
[[nodiscard]] ExposureAt exposure_at(const ShoreExposure& exposure, glm::vec2 world_xz);

/// What the ground does to the sea at one point: a factor on each sum, how
/// that factor changes across the point, and how hard the water is breaking.
///
/// Both sums of the sea — the Gerstner trains and the simulated sea — arrive
/// with the share the land in front of them lets through, grow by their own
/// dominant wave's coefficient, and breaking then holds them to what the depth
/// can carry. Breaking is a limit on how tall the water is, so it takes the
/// two together: their heights add as variances, and one ratio scales both.
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
/// `still` deep and `exposure` open, or nothing where it has none: where every
/// sum present is in deep water and wholly open to the sea, and on a flat sea.
/// Nothing means the sea is left exactly as it is, not scaled by 1. With no
/// depth — off the terrain, where a lee can still reach — the water counts as
/// deep: sheltered, not shoaled.
[[nodiscard]] std::optional<ShoreEffect> shore_effect(const std::optional<StillDepth>& still,
                                                      const ExposureAt& exposure,
                                                      ShoalingSum gerstner,
                                                      ShoalingSum simulated);

/// Steepest beach the swash formulas are taken for: past it the ground is a
/// rock face or a cliff, which takes spray rather than setup.
inline constexpr float SWASH_STEEPEST_BEACH = 0.15f;

/// How the water level at one point rides the waves arriving at the beach:
/// what it takes, before the arriving sea is known.
///
/// Broken waves run on up the sand as a sheet and drain back, so the water's
/// edge moves up and down the beach with them, and the mean level across the
/// surf stands raised (setup). Both are a vertical lift of the still level
/// near the waterline; where the water's edge lands is then the terrain's.
/// The swash follows the sea arriving at the still waterline straight down the
/// beach from this point, read there as a share of its significant amplitude,
/// so the edge runs up with the breakers that are drawn arriving.
///
/// Heights after Stockdon et al. (2006), from the sea's sheltered significant
/// height H0, its wavelength L0 and the beach slope: setup
/// `0.35 β sqrt(H0 L0)`, and the level swinging half of
/// `sqrt(H0 L0 (0.563 β² + 0.004))` either side of it.
struct SwashPlan {
    /// Where on the still waterline the arriving sea is read.
    glm::vec2 waterline{0.0f};
    /// The way down the beach, seaward.
    glm::vec2 seaward{1.0f, 0.0f};
    /// Metres the mean level stands raised here, and how that changes across
    /// the point: all of it at and above the waterline, falling to nothing
    /// at the depth the sea breaks in.
    float setup{0.0f};
    glm::vec2 setup_gradient{0.0f};
    /// Metres the level swings either side of that with the sea arriving in
    /// full, and how that changes: all of it at and above the waterline,
    /// falling to nothing in water twice as deep as the swing.
    float swing{0.0f};
    glm::vec2 swing_gradient{0.0f};
    /// One over the significant amplitude of the sea as it arrives, before
    /// the shore scales it: the arriving height times this is its share.
    float scale{0.0f};
    /// How much of the swing this point carries, 0 to 1: what a material reads
    /// as SWASH.
    float share{0.0f};
};

/// The swash at `world_xz` over `bed` for a sea of `gerstner` and `simulated`,
/// or nothing where there is none: off the terrain, on level ground, in water
/// deeper than the sea breaks in, and on sand higher than it can ever reach.
[[nodiscard]] std::optional<SwashPlan> swash_plan(const ShoreBed& bed, ShoalingSum gerstner,
                                                  ShoalingSum simulated, glm::vec2 world_xz);

/// The lift a plan comes to with the sea arriving at its waterline at
/// `sea_height` (before the shore scaled it), its slope there `sea_slope` and
/// its rate of rise `sea_rate`: the lift, how it changes across the point and
/// how fast it rises.
///
/// Every point down one line of the beach reads the same sample, so across the
/// point the arriving sea changes only along the shore: its slope there is
/// taken without the part down the beach.
struct Swash {
    float lift{0.0f};
    glm::vec2 gradient{0.0f};
    float rate{0.0f};
};
[[nodiscard]] Swash swash_from(const SwashPlan& plan, float sea_height, glm::vec2 sea_slope,
                               float sea_rate);

/// The still-water depth at world `world_xz`, or nothing where the bed is not
/// known: off the terrain, or with no terrain at all. Nothing is open water —
/// no bed near enough to matter — and not a depth of zero.
///
/// Read bilinearly between samples, the way the terrain draws itself, so the
/// waterline is where the drawn ground meets the water.
[[nodiscard]] std::optional<StillDepth> still_depth(const ShoreBed& bed, glm::vec2 world_xz);

} // namespace water
} // namespace fjell
