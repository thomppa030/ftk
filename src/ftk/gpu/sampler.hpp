#pragma once

#include "ftk/base/handle.hpp"
#include "ftk/gpu/compare.hpp"

#include <cstdint>
#include <optional>

namespace ftk::gpu {

struct SamplerTag;

/// A sampler, by handle. `Device::sampler()` gives the same one for the same
/// description, and it lasts as long as the device: nobody destroys it.
using Sampler = Handle<SamplerTag>;

/// How texels are filtered.
enum class Filter : uint8_t {
    nearest,
    linear,
};

/// What a coordinate outside 0..1 reads.
enum class Address : uint8_t {
    repeat,
    mirror,
    /// The edge texel.
    clamp,
    /// `SamplerDesc::border`.
    border,
};

/// The colour `Address::border` reads: the three both Vulkan and Metal have.
enum class Border : uint8_t {
    transparent_black,
    opaque_black,
    opaque_white,
};

/// What a sampler is made from. Two equal descriptions are the same sampler.
///
/// @code
/// gpu::Sampler linear_clamp = device.sampler({.address = gpu::Address::clamp});
/// gpu::Sampler shadow = device.sampler({.address = gpu::Address::clamp,
///                                       .compare = gpu::Compare::less_equal});
/// @endcode
struct SamplerDesc {
    /// Filtering when magnified and minified.
    Filter filter{Filter::linear};
    /// Filtering between mip levels.
    Filter mip_filter{Filter::linear};
    /// Addressing on every axis.
    Address address{Address::repeat};
    /// Anisotropic filtering up to this many samples, capped at what the device
    /// allows; 0 or 1 is none.
    float max_anisotropy{0.0f};
    /// Makes a comparison sampler, which returns how many samples pass.
    std::optional<Compare> compare{};
    Border border{Border::transparent_black};
    /// Mip levels the sampler may read, as a range of level of detail.
    float min_lod{0.0f};
    float max_lod{1000.0f};

    bool operator==(const SamplerDesc&) const = default;
};

} // namespace ftk::gpu
