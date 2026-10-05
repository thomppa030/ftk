#pragma once

#include <cstdint>
#include <vector>

namespace ftk::image {

/// What a filter finds past an image's edge.
enum class Edge : uint8_t {
    wrap,  ///< the opposite edge, for a texture that tiles
    clamp, ///< the edge texel again, for an image that ends there
};

/// An image of float texels, `channels` to a texel, packed row after row.
struct FloatImage {
    std::vector<float> texels{};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t channels{4};
};

/// `image` resized to `width` × `height` (each at least 1) with a
/// Kaiser-windowed sinc, one axis after the other. Shrunk, it keeps detail a
/// box filter blurs away; the price is a little ringing past hard edges, so
/// values can land slightly outside the range the image had. Every output
/// texel's weights add up to one, so a flat image stays exactly as it was.
/// Channels are filtered as they are: an image in sRGB should be made linear
/// first.
[[nodiscard]] FloatImage resize(const FloatImage& image, uint32_t width, uint32_t height, Edge edge);

} // namespace ftk::image
