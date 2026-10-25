#pragma once

#include <cstdint>
#include <vector>

namespace ftk::image {

/// An image as RGBA, four bytes a texel, row after row from the top, its alpha
/// not multiplied into its colours.
struct Rgba8 {
    std::vector<uint8_t> texels;
    uint32_t width{0};
    uint32_t height{0};
};

} // namespace ftk::image
