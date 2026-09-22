#pragma once

#include "renderer/resources/fjwater.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace fjell::water {

/// JONSWAP energy density at angular frequency `omega` (rad/s), in m²·s: how
/// much of the sea's height variance sits at that frequency, for the
/// contributor's wind and fetch. Integrated over all frequencies it is the
/// variance of the surface height.
///
/// Before any stylisation: `high_cut` and `amplitude_scale` are not applied.
/// 0 for a non-positive frequency, and for no wind or no fetch.
[[nodiscard]] float jonswap(const FftContributor& sea, float omega);

/// Half the significant wave height of the sea the contributor describes, in
/// metres: the amplitude of the waves an observer would call its size. 0 for
/// no wind or no fetch.
///
/// From the spectrum's variance in closed form — a JONSWAP spectrum carries
/// 1.52 times the energy of the Pierson-Moskowitz one under it, which
/// integrates exactly — so it is cheap enough to ask every frame. The high cut
/// is not accounted for; it removes the short waves, which hold little of the
/// height.
[[nodiscard]] float significant_amplitude(const FftContributor& sea);

/// The frequency the spectrum peaks at, in rad/s, or 0 for no wind or no
/// fetch. More wind or more fetch lowers it: a more grown sea is a longer one.
[[nodiscard]] float peak_frequency(const FftContributor& sea);

/// How the energy at one frequency is shared out by direction, as a density
/// over `angle_from_wind` in radians. Integrates to 1 around the circle, so
/// it moves energy between directions without adding or removing any.
[[nodiscard]] float directional_spread(const FftContributor& sea, float angle_from_wind);

/// Energy density over the wavevector plane at `k` (rad/m, world XZ), in m⁴,
/// with every stylisation applied. Summed over a grid of wavevectors and
/// multiplied by a cell's area it is the variance the grid holds. 0 at k = 0.
[[nodiscard]] float wavenumber_spectrum(const FftContributor& sea, glm::vec2 k);

/// The random part of a drawn sea: a pair of independent standard normals for
/// every wavevector of an N x N grid, row-major.
///
/// It depends on the seed and the resolution and on nothing else, which is
/// what lets a sea change its wind without changing which sea it is: the same
/// noise under a new spectrum is the same waves, grown or turned, rather than
/// a fresh roll of them. It is also most of what drawing a sea costs, so
/// whoever redraws one often keeps this and reuses it.
struct SpectrumNoise {
    std::uint32_t resolution{0};
    std::uint32_t seed{0};
    std::vector<glm::vec2> normals;
};

/// Draw the noise for a seed. The resolution is rounded down to a power of
/// two; under 2 the noise comes back empty. Deterministic on every platform.
[[nodiscard]] SpectrumNoise draw_noise(std::uint32_t seed, std::uint32_t resolution);

/// Draw a sea from the contributor's spectrum.
///
/// Each amplitude is a complex Gaussian scaled so that the surface the grid
/// animates into has the variance the spectrum says, once every wavevector is
/// paired with its opposite to make the heights real. Deterministic in the
/// seed on every platform. The resolution is rounded down to a power of two;
/// an empty field comes back for a resolution under 2 or a patch of no size.
[[nodiscard]] SpectrumField generate_spectrum(const FftContributor& sea);

/// The same, from noise already drawn. `noise` must be the one for this
/// contributor's seed and resolution; given any other, the field comes back
/// empty rather than shaped from the wrong sea.
[[nodiscard]] SpectrumField generate_spectrum(const FftContributor& sea,
                                              const SpectrumNoise& noise);

/// The three spectra a simulated sea is transformed from at `time`: height,
/// and the horizontal displacement along x and z that pinches its crests.
/// Same layout as `SpectrumField::amplitudes`.
///
/// Every wavevector turns at its deep-water frequency, and is summed with the
/// conjugate of its opposite so that the transform of each is real. The
/// horizontal spectra are the height's, turned a quarter cycle and pointed
/// along the wavevector — the motion that carries water toward a crest, which
/// is what a Gerstner train's steepness buys — and scaled by `choppiness`.
struct DisplacementSpectra {
    std::vector<glm::vec2> height;
    std::vector<glm::vec2> along_x;
    std::vector<glm::vec2> along_z;
};
[[nodiscard]] DisplacementSpectra evolve_spectrum(const SpectrumField& field, float choppiness,
                                                  float time);

/// The inverse discrete Fourier transform of an N x N spectrum in FFT order,
/// keeping the real part: `out[z * N + x]` is the value at patch position
/// (x, z) * patch_size / N.
///
/// A plain sum, a term per wavevector per sample, and so far too slow to draw
/// a sea with. It is here as the definition the GPU's fast transform has to
/// reproduce: there is no cleverness in it to be wrong.
[[nodiscard]] std::vector<float> inverse_transform_reference(const std::vector<glm::vec2>& spectrum,
                                                             std::uint32_t resolution);

/// The same transform as `inverse_transform_reference()`, fast: a radix-2 FFT
/// along each axis, N^2 log N where the plain sum is N^4. For CPU work that
/// needs the surface at its full resolution — measuring it, not drawing it.
/// `resolution` must be a power of two; anything else, or a spectrum of the
/// wrong size, gives back zeros.
[[nodiscard]] std::vector<float> inverse_transform(const std::vector<glm::vec2>& spectrum,
                                                   std::uint32_t resolution);

/// How the simulated sea's horizontal displacement changes across the patch
/// at `time`: the entries it adds to the surface's horizontal Jacobian, exact,
/// at every grid point. Same layout as the transforms' output.
///
/// These are what decide whether the surface has folded, summed with any
/// other contributor's before the determinant is taken. Exact rather than the
/// texel differences the shader shades with, because a fold is a fact about
/// the surface and not about how it is lit.
struct JacobianFields {
    std::vector<float> xx;   // d(displacement x)/dx
    std::vector<float> zz;   // d(displacement z)/dz
    std::vector<float> xz;   // d(displacement x)/dz, which is also d(displacement z)/dx
};
[[nodiscard]] JacobianFields jacobian_fields(const SpectrumField& field, float choppiness,
                                             float time);

/// The strongest waves of a drawn sea, and what taking only those leaves out.
struct StrongestWaves {
    /// Strongest first. Waves of equal strength keep their grid order, so the
    /// same spectrum gives the same list everywhere.
    std::vector<SpectralWave> waves;
    /// How much of the sea's height these carry, 0 to 1: the root of their
    /// share of its variance, since height goes as the standard deviation.
    float height_share{0.0f};
    /// The shortest wavelength among them, in metres; 0 when there are none.
    /// Everything shorter than this is what a body floating on them misses.
    float shortest_wavelength{0.0f};
};

/// The `count` strongest wavevectors of `field`, or all that can be taken when
/// there are fewer. The grid's highest frequency along either axis is never
/// taken — it is a standing ripple rather than a travelling wave — so
/// `height_share` stops a hair short of 1 even when everything else is.
///
/// The weaker half of a cut set is tapered by rank, down to nothing at the
/// cutoff. A sea under the weather is redrawn each time the wind steps and its
/// strongest waves picked again; a wave near the cutoff then enters or leaves
/// the set, and taken whole it would step the surface a body feels by its own
/// amplitude where the sea itself barely moved. Tapered, a wave arrives at
/// nothing and grows as it climbs the ranks. A set that is not cut — every
/// wave taken — has no cutoff and no taper. `height_share` counts the waves as
/// tapered.
[[nodiscard]] StrongestWaves strongest_waves(const SpectrumField& field, std::uint32_t count);

/// `waves` laid out for the vector unit. `PackedWaves` says why.
[[nodiscard]] PackedWaves pack_waves(const std::vector<SpectralWave>& waves);

/// Where `waves` have moved the surface at rest position `world_xz`: x and z
/// the horizontal pinch toward crests, scaled by `choppiness`, y the height.
/// The closed-form counterpart of transforming `evolve_spectrum()`, and equal
/// to it at the grid's sample points when `waves` is every wave that
/// `strongest_waves()` will take.
[[nodiscard]] glm::vec3 spectral_displacement(const std::vector<SpectralWave>& waves,
                                              float choppiness, glm::vec2 world_xz, float time);
[[nodiscard]] glm::vec3 spectral_displacement(const PackedWaves& waves, float choppiness,
                                              glm::vec2 world_xz, float time);

/// `spectral_displacement()` and its exact time derivative from one pass over
/// the waves.
struct SpectralMotion {
    glm::vec3 displacement{0.0f};
    glm::vec3 velocity{0.0f};
};
[[nodiscard]] SpectralMotion spectral_motion(const std::vector<SpectralWave>& waves,
                                             float choppiness, glm::vec2 world_xz, float time);
[[nodiscard]] SpectralMotion spectral_motion(const PackedWaves& waves, float choppiness,
                                             glm::vec2 world_xz, float time);

/// How the surface `waves` make changes across the rest plane at `world_xz`:
/// the height's slopes, and the entries the horizontal pinch adds to the
/// surface's Jacobian. Exact, from the same closed form as
/// `spectral_displacement()`.
struct SpectralDerivatives {
    float dh_dx{0.0f};
    float dh_dz{0.0f};
    float xx{0.0f};   // d(displacement x)/dx
    float zz{0.0f};   // d(displacement z)/dz
    float xz{0.0f};   // d(displacement x)/dz, which is also d(displacement z)/dx
};
[[nodiscard]] SpectralDerivatives spectral_derivatives(const std::vector<SpectralWave>& waves,
                                                       float choppiness, glm::vec2 world_xz,
                                                       float time);
[[nodiscard]] SpectralDerivatives spectral_derivatives(const PackedWaves& waves, float choppiness,
                                                       glm::vec2 world_xz, float time);

} // namespace fjell::water
