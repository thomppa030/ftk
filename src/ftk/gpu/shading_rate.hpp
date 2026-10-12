#pragma once

#include "ftk/gpu/format.hpp"

#include <array>
#include <cstdint>

namespace ftk::gpu {

/// How many pixels one run of a fragment shader covers, across and down. 1 × 1
/// shades every pixel; 2 × 2 runs once for each 2 × 2 block of pixels a
/// surface covers and gives all four the one result. Coverage and depth stay
/// per sample at any rate, so edges stay where they are; only the shading
/// inside a surface gets coarser. Each side is 1, 2 or 4.
struct ShadingRate {
    uint8_t width{1};
    uint8_t height{1};

    friend constexpr bool operator==(ShadingRate, ShadingRate) = default;
};

/// What a GPU offers for shading in blocks of pixels. A GPU without any of it
/// reports neither switch and offers 1 × 1 alone.
struct ShadingRates {
    /// Whether a draw may set a rate of its own.
    bool per_draw{false};
    /// Whether a render scope may hold a rate image: one rate for each tile of
    /// pixels, written before the scope draws.
    bool image{false};
    /// The pixels across and down that one texel of a rate image covers, the
    /// smallest the GPU allows: 16 × 16 on NVIDIA, 8 × 8 on most others. 0
    /// without an image.
    uint32_t tile_width{0};
    uint32_t tile_height{0};
    /// For each rate, the sample counts it is offered at, one bit per count as
    /// `Samples` values them; indexed by the width's and the height's log2.
    /// The rates a GPU offers depend on the samples: under 4× MSAA a 2 × 2
    /// block covers 16 samples, which some GPUs shade at most at once.
    std::array<std::array<uint8_t, 3>, 3> offered_at{};

    /// Whether `rate` is offered to a draw with `samples` per pixel. 1 × 1
    /// always is.
    [[nodiscard]] bool offers(ShadingRate rate, Samples samples) const;
    /// Records that `rate` is offered at `samples`; the backend fills the
    /// table from what the GPU reports. A side other than 1, 2 or 4 is
    /// ignored.
    void offer(ShadingRate rate, Samples samples);
};

/// The rate a draw with `samples` per pixel gets when it asks for `asked`,
/// chosen as a GPU chooses for a rate it does not offer: of the offered rates
/// no wider and no taller than `asked`, the largest, then the squarest (the
/// least width + height); of a wide and a tall one left, the wide one, where a
/// GPU may take either. Asking only for rates this returns, nothing is left
/// for the GPU to choose, so what is drawn is what was asked.
[[nodiscard]] ShadingRate rate_for(const ShadingRates& rates, Samples samples, ShadingRate asked);

/// What a draw shades at: a rate of its own, or the rate image's where the
/// render scope holds one (and 1 × 1 where it holds none).
struct DrawShading {
    ShadingRate rate{};
    bool from_image{false};

    friend constexpr bool operator==(DrawShading, DrawShading) = default;
};

/// What a GPU is told for a draw asking `asked` among targets with `samples`:
/// a rate of its own becomes one the GPU offers there (`rate_for`), and 1 × 1
/// where it sets no rates per draw; the image is taken only where it has
/// images.
[[nodiscard]] DrawShading shading_for(const ShadingRates& rates, Samples samples, DrawShading asked);

} // namespace ftk::gpu
