#pragma once

#include "ftk/base/result.hpp"
#include "ftk/image/rgba8.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ftk::image {

/// The sizes a program's icon is drawn at, largest first: what title bars,
/// taskbars, window switchers and Explorer ask for.
inline constexpr std::array<uint32_t, 7> ICON_SIZES{256, 128, 64, 48, 32, 24, 16};

/// The SVG file `svg`, a square, drawn at each of ICON_SIZES; why not when it
/// doesn't draw.
[[nodiscard]] Result<std::vector<Rgba8>> rasterize_icon(std::string_view svg);

/// The pictures `sizes` as the bytes of an ICO file, the icon a Windows
/// program carries: one entry each, in the order given, its picture held as a
/// PNG, as Windows reads them since Vista. Empty unless every picture is
/// square and from 1 to 256 pixels a side.
[[nodiscard]] std::vector<std::byte> encode_ico(std::span<const Rgba8> sizes);

} // namespace ftk::image
