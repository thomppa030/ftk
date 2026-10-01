#pragma once

#include "ftk/gpu/flags.hpp"

#include <cstdint>

namespace fjell::gpu {

/// How a texture may be used. Copies and clears are always allowed and never
/// spelled.
enum class TextureUse : uint8_t {
    /// Read through a sampler or fetched in a shader.
    sampled,
    /// Read or written as a storage image.
    storage,
    /// Rendered to as a colour attachment.
    color_target,
    /// Rendered to as a depth (and stencil) attachment.
    depth_target,
};

template <>
inline constexpr bool is_flag_enum<TextureUse> = true;

/// A set of `TextureUse`s.
using TextureUses = Flags<TextureUse>;

/// How a buffer may be used. Copies and fills are always allowed and never
/// spelled.
enum class BufferUse : uint8_t {
    uniform,
    storage,
    vertex,
    index,
    /// Holds the arguments of indirect draws or dispatches.
    indirect,
    /// Holds vertices, indices or instances an acceleration structure is built from.
    acceleration_input,
    /// Its address is taken and read by shaders.
    device_address,
};

template <>
inline constexpr bool is_flag_enum<BufferUse> = true;

/// A set of `BufferUse`s.
using BufferUses = Flags<BufferUse>;

} // namespace fjell::gpu
