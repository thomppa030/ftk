#pragma once

#include <cstdint>

namespace fjell::gpu {

/// A texel format. Only the formats the engine uses are here, each with a
/// mapping in every backend. Formats Apple GPUs lack (24-bit depth with
/// stencil, three-channel colour) have no place here, so nothing can come to
/// depend on one.
enum class Format : uint8_t {
    undefined,
    r8_unorm,
    rg8_unorm,
    rgba8_unorm,
    rgba8_srgb,
    bgra8_unorm,
    bgra8_srgb,
    r16_unorm,
    rg16_unorm,
    rgba16_unorm,
    r16_float,
    rg16_float,
    rgba16_float,
    r32_float,
    r32_uint,
    rgba32_float,
    d32_float,
    d32_float_s8_uint,
};

/// What a format's texels hold: which parts of the texture a view reaches, and
/// how a clear value is read.
enum class FormatKind : uint8_t {
    /// `Format::undefined`.
    none,
    /// Colour stored as floats or normalised integers, read as floats.
    color,
    /// Colour stored and read as unsigned integers.
    color_uint,
    depth,
    depth_stencil,
};

/// What `format`'s texels hold.
[[nodiscard]] constexpr FormatKind kind(Format format) noexcept {
    switch (format) {
        case Format::undefined:
            return FormatKind::none;
        case Format::r32_uint:
            return FormatKind::color_uint;
        case Format::d32_float:
            return FormatKind::depth;
        case Format::d32_float_s8_uint:
            return FormatKind::depth_stencil;
        case Format::r8_unorm:
        case Format::rg8_unorm:
        case Format::rgba8_unorm:
        case Format::rgba8_srgb:
        case Format::bgra8_unorm:
        case Format::bgra8_srgb:
        case Format::r16_unorm:
        case Format::rg16_unorm:
        case Format::rgba16_unorm:
        case Format::r16_float:
        case Format::rg16_float:
        case Format::rgba16_float:
        case Format::r32_float:
        case Format::rgba32_float:
            return FormatKind::color;
    }
    return FormatKind::none;
}

/// Bytes of one texel of `format` as it is copied to or from a buffer; 0 for
/// `Format::undefined` and for depth with stencil, which is copied an aspect
/// at a time.
[[nodiscard]] constexpr uint32_t texel_size(Format format) noexcept {
    switch (format) {
        case Format::undefined:
        case Format::d32_float_s8_uint:
            return 0;
        case Format::r8_unorm:
            return 1;
        case Format::rg8_unorm:
        case Format::r16_unorm:
        case Format::r16_float:
            return 2;
        case Format::rgba8_unorm:
        case Format::rgba8_srgb:
        case Format::bgra8_unorm:
        case Format::bgra8_srgb:
        case Format::rg16_unorm:
        case Format::rg16_float:
        case Format::r32_float:
        case Format::r32_uint:
        case Format::d32_float:
            return 4;
        case Format::rgba16_unorm:
        case Format::rgba16_float:
            return 8;
        case Format::rgba32_float:
            return 16;
    }
    return 0;
}

/// A vertex attribute's format, kept apart from `Format` as Metal keeps its
/// vertex formats apart from its pixel formats.
enum class VertexFormat : uint8_t {
    float2,
    float3,
    float4,
    uint4,
};

/// Samples per pixel of a render target, valued by their count. Which counts
/// a device supports is in its capabilities (Apple GPUs before M5 stop at 4).
enum class Samples : uint8_t {
    x1 = 1,
    x2 = 2,
    x4 = 4,
    x8 = 8,
};

} // namespace fjell::gpu
