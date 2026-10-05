#pragma once

#include "ftk/image/resample.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ftk::image {

/// What an image's texels hold, which decides how its mips are filtered.
enum class MipContent : uint8_t {
    /// Colour in sRGB, with alpha (the fourth channel, or the second of two)
    /// linear: filtered in linear light, so mips do not darken.
    srgb_color,
    /// Values filtered as they are: data such as roughness or masks, and
    /// colour above 1 in linear light.
    linear,
    /// A tangent-space normal in the first three channels, each from 0 to 1
    /// for -1 to 1: filtered as vectors and made unit length again.
    normal_map,
};

struct MipOptions {
    MipContent content{MipContent::srgb_color};
    Edges edges{};
    /// For a cutout, the alpha under which its surface is cut away. Each
    /// mip's alpha is then scaled so the share of texels at or above it stays
    /// what it is in the image, and the cutout keeps its coverage at a
    /// distance. The image needs an alpha channel for it.
    std::optional<float> cutout{};
};

/// The mips after `image`, largest first, each half the size of the one
/// before rounded down and at least 1, down to 1 × 1. Each is filtered from
/// the one before (resize()), and holds what the image holds as the image
/// holds it: sRGB colour in, sRGB colour out, clamped to 0 to 1, and a normal
/// map's vectors unit length. Empty for an image of 1 × 1.
[[nodiscard]] std::vector<FloatImage> make_mips(const FloatImage& image, const MipOptions& options);

/// `image` made `width` × `height` the way its mips are made: colour in
/// linear light, normals unit length again, a cutout's coverage kept. For a
/// texture brought under a largest size, by any ratio.
[[nodiscard]] FloatImage shrink(const FloatImage& image, uint32_t width, uint32_t height, const MipOptions& options);

/// The share of `image`'s texels whose alpha is at or above `cutoff`.
[[nodiscard]] float alpha_coverage(const FloatImage& image, float cutoff);

} // namespace ftk::image
