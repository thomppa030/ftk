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

/// One sea drawn from a spectrum: the complex amplitude of every wavevector
/// on the grid at time zero.
struct SpectrumField {
    /// Samples per side.
    std::uint32_t resolution{0};

    /// Spacing of the wavevector grid in rad/m: 2*pi / patch_size.
    float k_spacing{0.0f};

    /// Amplitudes, real in x and imaginary in y, row-major as `[z * resolution
    /// + x]`. In FFT order: index i along an axis is wavenumber i for the
    /// first half and i - resolution for the second, so the mean sits at 0.
    std::vector<glm::vec2> amplitudes;

    /// The wavevector index `i` along one axis stands for, in rad/m.
    [[nodiscard]] float wavenumber(std::uint32_t i) const;
};

/// Draw a sea from the contributor's spectrum.
///
/// Each amplitude is a complex Gaussian scaled so that the surface the grid
/// animates into has the variance the spectrum says, once every wavevector is
/// paired with its opposite to make the heights real. Deterministic in the
/// seed on every platform. The resolution is rounded down to a power of two;
/// an empty field comes back for a resolution under 2 or a patch of no size.
[[nodiscard]] SpectrumField generate_spectrum(const FftContributor& sea);

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

} // namespace fjell::water
