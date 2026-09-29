#pragma once

#include "core/handle.hpp"
#include "gpu/clear.hpp"
#include "gpu/format.hpp"
#include "gpu/usage.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fjell::gpu {

class Device;
struct TextureTag;

/// A texture, by handle. Made by `Device::create`, held by `Owned<Texture>`.
using Texture = Handle<TextureTag>;

/// Hands a texture back to its device, which destroys it once the GPU is done
/// with it. `Owned<Texture>` calls it.
void release(Device& device, Texture texture);

/// The shape of a texture.
enum class TextureKind : uint8_t {
    tex2d,
    tex2d_array,
    /// Six square layers, one per face.
    cube,
    tex3d,
};

/// Which queues touch a texture. Vulkan shares a texture across queue families
/// only when told; Metal ignores it.
enum class Queues : uint8_t {
    graphics,
    /// Also the async compute queue.
    graphics_and_compute,
};

/// What `Device::create` makes a texture from.
///
/// @code
/// auto map = device.create(gpu::TextureDesc{
///     .format = gpu::Format::r8_unorm,
///     .width = 256, .height = 256,
///     .use = gpu::TextureUse::storage | gpu::TextureUse::sampled,
///     .initial = gpu::Clear{1.0f},
///     .name = "cloud_shadow",
/// });
/// @endcode
struct TextureDesc {
    TextureKind kind{TextureKind::tex2d};
    Format format{Format::undefined};
    uint32_t width{1};
    uint32_t height{1};
    /// Depth of a 3D texture; 1 for any other.
    uint32_t depth{1};
    /// Layers of a 2D array; a cube always has six, any other kind one.
    /// More than one on any other kind is refused.
    uint32_t layers{1};
    uint32_t mips{1};
    Samples samples{Samples::x1};
    TextureUses use{};
    Queues queues{Queues::graphics};
    /// What the texture holds before anything writes it. Cleared on the device's
    /// upload lane, which runs before the next frame, after which the texture
    /// is ready to be sampled, or, without `TextureUse::sampled`, read and
    /// written as storage. Left out, it holds nothing defined.
    std::optional<Clear> initial{};
    /// Shown by debuggers and validation messages; not kept.
    std::string_view name{};
};

/// What a texture was made as.
struct TextureInfo {
    TextureKind kind{TextureKind::tex2d};
    Format format{Format::undefined};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t depth{1};
    /// Layers as the device holds them: six for a cube.
    uint32_t layers{1};
    uint32_t mips{1};
    Samples samples{Samples::x1};
    TextureUses use{};
};

/// How a shader sees a view. `automatic` follows the texture and the range:
/// every layer (the default range) as the texture's own kind, a single layer
/// as 2D, a range of layers as a 2D array.
enum class ViewKind : uint8_t {
    automatic,
    tex2d,
    tex2d_array,
    cube,
    tex3d,
};

/// Part of a texture as a shader or an attachment sees it: a texture, a range
/// of mips and layers, and optionally another kind or format. A value, never
/// created or destroyed: the device makes what the backend needs the first
/// time a view is used and keeps it with the texture. A texture goes wherever
/// a view is taken, as the whole of itself.
///
/// @code
/// cmd.bind({{"dst_mip", gpu::storage(gpu::mip(pyramid_, level))}});
/// gpu::TextureView faces = sky_;                 // the whole cube
/// faces.kind = gpu::ViewKind::tex2d_array;       // as six layers for a compute write
/// @endcode
struct TextureView {
    /// `mip_count` or `layer_count` reaching to the texture's last.
    static constexpr uint32_t REST = UINT32_MAX;

    Texture texture{};
    uint32_t base_mip{0};
    uint32_t mip_count{REST};
    uint32_t base_layer{0};
    uint32_t layer_count{REST};
    ViewKind kind{ViewKind::automatic};
    /// Another format of the same size to read the texels as; `undefined`
    /// keeps the texture's.
    Format format{Format::undefined};

    constexpr TextureView() = default;

    /// The whole texture, from a `Texture` or anything that holds one
    /// (`Owned<Texture>`).
    template <typename T>
        requires std::convertible_to<const T&, Texture>
    constexpr TextureView(const T& whole) : texture(static_cast<Texture>(whole)) {}

    bool operator==(const TextureView&) const = default;
};

/// One mip level of a texture, every layer.
[[nodiscard]] constexpr TextureView mip(Texture texture, uint32_t level) {
    TextureView view(texture);
    view.base_mip = level;
    view.mip_count = 1;
    return view;
}

/// One layer of an array or cube texture, every mip, seen as 2D.
[[nodiscard]] constexpr TextureView layer(Texture texture, uint32_t index) {
    TextureView view(texture);
    view.base_layer = index;
    view.layer_count = 1;
    return view;
}

/// One face of a cube texture (0..5: +x, -x, +y, -y, +z, -z), seen as 2D.
[[nodiscard]] constexpr TextureView face(Texture texture, uint32_t index) {
    return layer(texture, index);
}

/// A view with every choice made: the counts, the kind and the format a
/// backend creates it with.
struct ResolvedView {
    uint32_t base_mip{0};
    uint32_t mip_count{1};
    uint32_t base_layer{0};
    uint32_t layer_count{1};
    ViewKind kind{ViewKind::tex2d};
    Format format{Format::undefined};

    bool operator==(const ResolvedView&) const = default;
};

/// `view` of a texture made as `info`, with `REST` counted out, the kind
/// chosen and the format filled in. Two views that resolve alike are the same
/// view.
[[nodiscard]] constexpr ResolvedView resolve(const TextureView& view, const TextureInfo& info) {
    ResolvedView out;
    out.base_mip = view.base_mip;
    out.mip_count = view.mip_count == TextureView::REST ? info.mips - view.base_mip : view.mip_count;
    out.base_layer = view.base_layer;
    out.layer_count = view.layer_count == TextureView::REST ? info.layers - view.base_layer
                                                            : view.layer_count;
    out.format = view.format == Format::undefined ? info.format : view.format;

    const bool whole = view.base_layer == 0 && view.layer_count == TextureView::REST;
    if (view.kind != ViewKind::automatic) {
        out.kind = view.kind;
    } else if (info.kind == TextureKind::tex3d) {
        out.kind = ViewKind::tex3d;
    } else if (whole) {
        switch (info.kind) {
            case TextureKind::tex2d:       out.kind = ViewKind::tex2d; break;
            case TextureKind::tex2d_array: out.kind = ViewKind::tex2d_array; break;
            case TextureKind::cube:        out.kind = ViewKind::cube; break;
            case TextureKind::tex3d:       out.kind = ViewKind::tex3d; break;
        }
    } else if (out.layer_count == 1) {
        out.kind = ViewKind::tex2d;
    } else {
        out.kind = ViewKind::tex2d_array;
    }
    return out;
}

} // namespace fjell::gpu
