#pragma once

#include "ftk/base/handle.hpp"
#include "ftk/base/result.hpp"
#include "ftk/base/small_vector.hpp"
#include "ftk/gpu/acceleration.hpp"
#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/shader.hpp"
#include "ftk/gpu/texture.hpp"

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

/// What goes into one binding: a texture view, a sampler, a buffer range, a
/// texture view with its sampler, or an acceleration structure, as the
/// binding's kind asks.
struct BindResource {
    BindingKind kind{BindingKind::uniform_buffer};
    TextureView view{};
    Sampler sampler{};
    BufferRange buffer{};
    AccelerationStructure structure{};

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

/// An acceleration structure ray queries trace: `accelerationStructureEXT`.
[[nodiscard]] constexpr BindResource acceleration(AccelerationStructure structure) {
    return {.kind = BindingKind::acceleration_structure, .structure = structure};
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
///
/// For one the device makes (`Device::create`), each binding is named, as
/// its shared group's entries name it, and holds its `count` of elements;
/// the elements of an array may be left empty, and a shader's array sized at
/// run time reads it. `stages` are the stages that read it, or none for
/// every stage.
struct SharedLayoutDesc {
    std::string name;
    uint32_t usual_set{0};
    std::vector<ShaderBinding> bindings;
};

/// What `Device::create` makes a shared group from: resources for a shared
/// layout the device made, bound wherever a pipeline naming the layout takes
/// it. Every binding that is not an array is given; an array's elements may
/// be, or left empty for later updates. An entry giving an array element no
/// resource, every handle in it empty (`gpu::sampled({}, {})`), empties that
/// element, as an update does when what it held is going away.
///
/// @code
/// auto table = device.create(gpu::SharedGroupDesc{
///     .layout = bindless_layout_,
///     .entries = {{"materials", gpu::storage(materials_)},
///                 {"params", gpu::storage(params_)}},
///     .name = "bindless",
/// });
/// device.update(*table, {{{"textures", gpu::texture(albedo), 7}}});
/// @endcode
struct SharedGroupDesc {
    SharedLayout layout{};
    std::vector<BindEntry> entries{};
    /// Shown by debuggers, on every version of the group, and in error
    /// messages.
    std::string_view name{};
};

/// Places entries in a shared layout's group, each naming one of `layout`'s
/// bindings and, for an array, the element it fills. The group keeps what the
/// entries do not name, so unlike `place` they need not fill the set; an
/// element given twice is refused.
/// @return the placed entries in binding and element order, or which entry is
///         wrong (named).
[[nodiscard]] Result<PlacedSet> place_some(const SharedLayoutDesc& layout,
                                           std::span<const BindEntry> entries);

/// The set each of `shared` takes in `layout`, in the same order. A pipeline
/// names the shared layouts it binds; this finds where its shaders declare
/// them. A set fits a shared layout when every binding declared there is one
/// of the layout's by number and kind, an array (even of one element) only
/// where the layout has an array at least as long (one sized at run time fits
/// any). Names take no
/// part. Of the sets that fit, the layout's usual set is taken; else the only
/// one that fits.
/// @return the sets, or which layout fits no set, fits several none of which
///         is its usual set, or would share a set with another.
[[nodiscard]] Result<std::vector<uint32_t>> place_shared(const ShaderLayout& layout,
                                                         std::span<const SharedLayoutDesc> shared);

} // namespace fjell::gpu
