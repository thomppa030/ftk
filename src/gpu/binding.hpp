#pragma once

#include "core/handle.hpp"
#include "core/result.hpp"
#include "core/small_vector.hpp"
#include "gpu/buffer.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/sampler.hpp"
#include "gpu/shader.hpp"
#include "gpu/texture.hpp"

#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fjell::gpu {

class Device;
struct BindGroupTag;

/// A set of resources bound together at one set of a pipeline, by handle.
using BindGroup = Handle<BindGroupTag>;

/// Hands a bind group back to its device. `Owned<BindGroup>` calls it.
void release(Device& device, BindGroup group);

/// Part of a buffer, or the whole of it.
struct BufferRange {
    /// `size` reaching to the buffer's end.
    static constexpr uint64_t REST = UINT64_MAX;

    Buffer buffer{};
    uint64_t offset{0};
    uint64_t size{REST};

    constexpr BufferRange() = default;

    /// The whole buffer, from a `Buffer` or anything that holds one
    /// (`Owned<Buffer>`).
    template <typename T>
        requires std::convertible_to<const T&, Buffer>
    constexpr BufferRange(const T& whole) : buffer(static_cast<Buffer>(whole)) {}

    constexpr BufferRange(Buffer whole, uint64_t from, uint64_t bytes)
        : buffer(whole), offset(from), size(bytes) {}

    bool operator==(const BufferRange&) const = default;
};

/// What goes into one binding: a texture view, a sampler, a buffer range, or
/// a texture view with its sampler, as the binding's kind asks.
struct BindResource {
    BindingKind kind{BindingKind::uniform_buffer};
    TextureView view{};
    Sampler sampler{};
    BufferRange buffer{};

    bool operator==(const BindResource&) const = default;
};

/// A texture read through a sampler: `sampler2D`.
[[nodiscard]] constexpr BindResource sampled(const TextureView& view, Sampler sampler) {
    return {.kind = BindingKind::sampled_texture, .view = view, .sampler = sampler};
}

/// A texture read with a sampler bound apart: `texture2D`.
[[nodiscard]] constexpr BindResource texture(const TextureView& view) {
    return {.kind = BindingKind::texture, .view = view};
}

/// A sampler bound apart from its texture: `sampler`.
[[nodiscard]] constexpr BindResource sampler(Sampler which) {
    return {.kind = BindingKind::sampler, .sampler = which};
}

/// A texture read or written as storage: `image2D`.
[[nodiscard]] constexpr BindResource storage(const TextureView& view) {
    return {.kind = BindingKind::storage_texture, .view = view};
}

/// A buffer read or written as storage: `buffer`.
[[nodiscard]] constexpr BindResource storage(const BufferRange& range) {
    return {.kind = BindingKind::storage_buffer, .buffer = range};
}

/// A uniform buffer: `uniform`.
[[nodiscard]] constexpr BindResource uniform(const BufferRange& range) {
    return {.kind = BindingKind::uniform_buffer, .buffer = range};
}

/// A resource for the binding the shader calls `name`, and for an array the
/// element it fills.
///
/// @code
/// cmd.bind({{"shadow_out", gpu::storage(map_)},
///           {"history", gpu::sampled(history_, linear_)}});
/// @endcode
struct BindEntry {
    std::string_view name;
    BindResource resource;
    uint32_t element{0};
};

/// What `Device::create` makes a persistent bind group from: resources for one
/// of a pipeline's own sets, which the entries' names pick. It binds to any
/// pipeline whose shaders declare that set alike.
///
/// @code
/// auto mip_set = device.create(gpu::BindGroupDesc{
///     .pipeline = hiz_build_,
///     .entries = {{"src_depth", gpu::sampled(gpu::mip(pyramid_, 2), nearest_)},
///                 {"dst_mip", gpu::storage(gpu::mip(pyramid_, 3))}},
/// });
/// @endcode
struct BindGroupDesc {
    PipelineRef pipeline;
    std::vector<BindEntry> entries;
    /// Shown by debuggers and in error messages; not kept.
    std::string_view name{};
};

/// One entry placed: its binding and element, checked against the shader.
struct PlacedEntry {
    uint32_t binding{0};
    uint32_t element{0};
    BindResource resource;
};

/// As many entries as a set holds without going to the heap: more than any
/// set the engine's shaders declare.
inline constexpr size_t INLINE_SET_ENTRIES = 16;

/// The entries of one set, placed and ordered by binding and element.
struct PlacedSet {
    uint32_t set{0};
    SmallVector<PlacedEntry, INLINE_SET_ENTRIES> entries;
};

/// Places `entries` in the set their names give in `layout`. Every entry must
/// name a binding of the same set, with a resource of the binding's kind, and
/// together they must fill that set: every binding, every element of an
/// array. A set holding an array sized at run time is a shared group's.
/// @return the placed set, or which entry or binding is wrong (named).
[[nodiscard]] Result<PlacedSet> place(const ShaderLayout& layout, std::span<const BindEntry> entries);

/// A set layout the engine shares between every pipeline that binds it (the
/// globals, the bindless table, draw data, meshlets), described by its
/// bindings. `usual_set` is the set most shaders declare it at.
struct SharedLayoutDesc {
    std::string name;
    uint32_t usual_set{0};
    std::vector<ShaderBinding> bindings;
};

/// The set each of `shared` takes in `layout`, in the same order. A pipeline
/// names the shared layouts it binds; this finds where its shaders declare
/// them. A set fits a shared layout when every binding declared there is one
/// of the layout's by number and kind, an array only where the layout has an
/// array at least as long (one sized at run time fits any). Names take no
/// part. Of the sets that fit, the layout's usual set is taken; else the only
/// one that fits.
/// @return the sets, or which layout fits no set, fits several none of which
///         is its usual set, or would share a set with another.
[[nodiscard]] Result<std::vector<uint32_t>> place_shared(const ShaderLayout& layout,
                                                         std::span<const SharedLayoutDesc> shared);

} // namespace fjell::gpu
