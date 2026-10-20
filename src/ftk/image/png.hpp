#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ftk::image {

/// `texels`, `channels` bytes each (1 grey, 2 grey and alpha, 3 RGB, 4 RGBA),
/// row after row from the top, as the bytes of a PNG file. Empty for texels
/// that don't fill `width` × `height`, and when they can't be encoded.
[[nodiscard]] std::vector<std::byte> encode_png(std::span<const uint8_t> texels, uint32_t width, uint32_t height,
                                                uint32_t channels);

} // namespace ftk::image
