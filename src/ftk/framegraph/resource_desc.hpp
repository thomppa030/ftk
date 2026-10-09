#pragma once

#include "ftk/gpu/access.hpp"
#include "ftk/gpu/format.hpp"
#include "ftk/gpu/texture.hpp"
#include "ftk/gpu/usage.hpp"

#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace ftk {

/// What a TextureDesc's size follows.
enum class SizeClass : uint8_t {
    absolute,       // width and height are exact pixels
    render,         // the view's render size, over `divisor`
    output,         // the view's output size, over `divisor`
};

/// The two sizes of a view: `render`, what its scene is drawn at, and
/// `output`, what it is shown at. The scene's images are render-sized up to
/// the temporal resolve, which writes at the output size.
struct ViewExtents {
    glm::uvec2 render{0, 0};
    glm::uvec2 output{0, 0};

    /// A view drawn at the size it is shown at.
    [[nodiscard]] static constexpr ViewExtents unscaled(glm::uvec2 size) { return {size, size}; }

    friend constexpr bool operator==(const ViewExtents&, const ViewExtents&) = default;
};

struct TextureDesc {
    /// Absolute unless the desc says it follows a view size; an absolute
    /// desc without a size is a mistake the graph reports.
    SizeClass size_class{SizeClass::absolute};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t depth{1};
    /// What a render- or output-sized texture divides the size by, rounded
    /// up: a half of an odd size covers its last pixel.
    uint32_t divisor{1};
    uint32_t mip_levels{1};
    uint32_t array_layers{1};

    gpu::Format format{gpu::Format::undefined};
    gpu::Samples samples{gpu::Samples::x1};
    gpu::TextureKind kind{gpu::TextureKind::tex2d};

    /// Uses beyond what the graph infers from pass declarations. Usually
    /// left empty: the graph adds the use of every gpu::Access declared
    /// against this resource.
    gpu::TextureUses extra_use{};

    /// Persistent resources survive across frames (shadow atlas, DDGI
    /// atlases, Hi-Z history, TAA history). Non-persistent resources are
    /// candidates for transient aliasing.
    bool persistent{false};
};

/// The size `desc` takes in a view of `extents`.
[[nodiscard]] inline glm::uvec3 resolved_size(const TextureDesc& desc, ViewExtents extents) {
    const uint32_t depth = desc.depth > 0 ? desc.depth : 1;
    const uint32_t divisor = desc.divisor > 0 ? desc.divisor : 1;
    switch (desc.size_class) {
        case SizeClass::render:
            return {glm::max((extents.render + (divisor - 1)) / divisor, glm::uvec2(1)), depth};
        case SizeClass::output:
            return {glm::max((extents.output + (divisor - 1)) / divisor, glm::uvec2(1)), depth};
        case SizeClass::absolute:
            break;
    }
    return {desc.width, desc.height, depth};
}

struct BufferDesc {
    uint64_t size{0};
    gpu::BufferUses extra_use{};
    bool persistent{false};
    bool host_visible{false};
};

/// Opaque handle into a FrameGraph. `id` is the graph-local resource index.
/// Handles are not interchangeable between graphs.
///
/// Named FgTexture / FgBuffer (not TextureHandle / BufferHandle) because
/// the runtime asset system in renderer/resources/resource_handle.hpp
/// already owns those names for persistent asset references. Framegraph
/// handles are a different thing: transient, per-frame, graph-scoped.
struct FgTexture {
    uint32_t id{UINT32_MAX};

    [[nodiscard]] constexpr bool valid() const noexcept { return id != UINT32_MAX; }
    friend constexpr bool operator==(FgTexture, FgTexture) noexcept = default;
};

struct FgBuffer {
    uint32_t id{UINT32_MAX};

    [[nodiscard]] constexpr bool valid() const noexcept { return id != UINT32_MAX; }
    friend constexpr bool operator==(FgBuffer, FgBuffer) noexcept = default;
};

/// An acceleration structure a pass declares, which the graph tracks as it
/// tracks a buffer: whole, by what was done to it last.
struct FgAcceleration {
    uint32_t id{UINT32_MAX};

    [[nodiscard]] constexpr bool valid() const noexcept { return id != UINT32_MAX; }
    friend constexpr bool operator==(FgAcceleration, FgAcceleration) noexcept = default;
};

/// Which queue the graph should schedule a pass on.
enum class QueueType : uint8_t {
    graphics,
    async_compute,
};

} // namespace ftk
