#pragma once

#include "ftk/base/result.hpp"
#include "ftk/image/rgba8.hpp"

#include <cstdint>
#include <string_view>

namespace ftk::image {

/// The most pixels an SVG is drawn at on either side.
inline constexpr uint32_t SVG_LARGEST = 8192;

/// An SVG's own width and height, in its own units.
struct SvgSize {
    float width{0.0f};
    float height{0.0f};
};

/// The size the SVG file `text` gives itself: its width and height, or its
/// viewBox's. An error, saying why, for text that isn't an SVG or an SVG with
/// no size.
[[nodiscard]] Result<SvgSize> svg_size(std::string_view text);

/// The SVG file `text` drawn into a picture `width` × `height` pixels: its
/// whole drawing (its own width and height, or its viewBox) stretched to
/// fill them, transparent where it draws nothing. Give `width` or `height` as
/// 0 and it follows from the other by the SVG's own proportions. The colours
/// are the SVG's own, sRGB as SVG writes them.
///
/// An error, saying why, for text that isn't an SVG, an SVG with no size, a
/// size of nothing, or a side larger than SVG_LARGEST.
[[nodiscard]] Result<Rgba8> rasterize_svg(std::string_view text, uint32_t width, uint32_t height);

} // namespace ftk::image
