#pragma once

#include <cstdint>

namespace ftk::gpu {

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
    r16_uint,
    r32_uint,
    rg8_uint,
    rg32_uint,
    rgba32_float,
    d32_float,
    d32_float_s8_uint,
    // Block-compressed: 4 × 4 texels a block, written whole from the CPU and
    // only ever sampled. Every desktop GPU and Apple silicon samples them
    // (`Caps::block_compression`).
    bc4_unorm,    // one channel
    bc5_unorm,    // two channels
    bc6h_ufloat,  // colour above 1, no alpha
    bc7_unorm,    // colour with alpha
    bc7_srgb,
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
        case Format::r16_uint:
        case Format::r32_uint:
        case Format::rg8_uint:
        case Format::rg32_uint:
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
        case Format::bc4_unorm:
        case Format::bc5_unorm:
        case Format::bc6h_ufloat:
        case Format::bc7_unorm:
        case Format::bc7_srgb:
            return FormatKind::color;
    }
    return FormatKind::none;
}

/// Whether `format` stores 4 × 4 blocks of texels rather than texels: a
/// texture of it is only sampled, and its mips are written, never filtered
/// down on the GPU.
[[nodiscard]] constexpr bool is_block_compressed(Format format) noexcept {
    switch (format) {
        case Format::bc4_unorm:
        case Format::bc5_unorm:
        case Format::bc6h_ufloat:
        case Format::bc7_unorm:
        case Format::bc7_srgb:
            return true;
        default:
            return false;
    }
}

/// Bytes of one 4 × 4 block of a block-compressed `format`; 0 for any other.
[[nodiscard]] constexpr uint32_t block_size(Format format) noexcept {
    switch (format) {
        case Format::bc4_unorm:
            return 8;
        case Format::bc5_unorm:
        case Format::bc6h_ufloat:
        case Format::bc7_unorm:
        case Format::bc7_srgb:
            return 16;
        default:
            return 0;
    }
}

/// Bytes of one texel of `format` as it is copied to or from a buffer; 0 for
/// `Format::undefined`, for depth with stencil, which is copied an aspect at a
/// time, and for the block-compressed formats, which are sized by the block
/// (`packed_size`).
[[nodiscard]] constexpr uint32_t texel_size(Format format) noexcept {
    switch (format) {
        case Format::undefined:
        case Format::d32_float_s8_uint:
        case Format::bc4_unorm:
        case Format::bc5_unorm:
        case Format::bc6h_ufloat:
        case Format::bc7_unorm:
        case Format::bc7_srgb:
            return 0;
        case Format::r8_unorm:
            return 1;
        case Format::rg8_unorm:
        case Format::r16_unorm:
        case Format::r16_float:
        case Format::r16_uint:
        case Format::rg8_uint:
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
        case Format::rg32_uint:
            return 8;
        case Format::rgba32_float:
            return 16;
    }
    return 0;
}

/// Bytes of a `width` × `height` × `depth` box of `format` packed as a buffer
/// holds it, row after row: whole blocks for a block-compressed format, a part
/// block at the box's edge taking a whole one. 0 for a format with no texel
/// size that is not block-compressed.
[[nodiscard]] constexpr uint64_t packed_size(Format format, uint32_t width, uint32_t height,
                                             uint32_t depth = 1) noexcept {
    if (is_block_compressed(format)) {
        return uint64_t{block_size(format)} * ((width + 3) / 4) * ((height + 3) / 4) * depth;
    }
    return uint64_t{texel_size(format)} * width * height * depth;
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

} // namespace ftk::gpu
