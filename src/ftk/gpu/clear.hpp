#pragma once

#include <array>
#include <cstdint>

namespace ftk::gpu {

/// What a texture is cleared to. The texture's format decides what is read:
/// a colour format reads `color` (an unsigned integer format as whole
/// numbers), a depth format reads `depth` and `stencil`.
///
/// @code
/// gpu::Clear{1.0f}                    // colour (1, 0, 0, 0)
/// gpu::Clear{0.1f, 0.1f, 0.1f, 1.0f}  // colour
/// gpu::Clear::depth_stencil(1.0f)     // depth 1, stencil 0
/// @endcode
struct Clear {
    std::array<float, 4> color{0.0f, 0.0f, 0.0f, 0.0f};
    float depth{1.0f};
    uint32_t stencil{0};

    constexpr Clear() = default;

    /// A colour, one to four channels; the ones left out are 0.
    constexpr explicit Clear(float r, float g = 0.0f, float b = 0.0f, float a = 0.0f)
        : color{r, g, b, a} {}

    /// A depth and a stencil value.
    [[nodiscard]] static constexpr Clear depth_stencil(float depth, uint32_t stencil = 0) {
        Clear clear;
        clear.depth = depth;
        clear.stencil = stencil;
        return clear;
    }
};

} // namespace ftk::gpu
