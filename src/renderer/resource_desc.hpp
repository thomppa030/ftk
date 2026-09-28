#pragma once

#include "gpu/access.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

/// Resolution of a TextureDesc at compile time.
/// "viewport" and fractions are resolved from the active viewport extent.
enum class SizeClass : uint8_t {
    absolute,       // width/height are exact pixels
    viewport,       // matches viewport extent (divisor applied)
    match,          // matches another resource (handled at compile)
};

struct TextureDesc {
    SizeClass size_class{SizeClass::viewport};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t depth{1};
    uint32_t viewport_divisor{1};
    uint32_t mip_levels{1};
    uint32_t array_layers{1};

    VkFormat format{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    VkImageViewType view_type{VK_IMAGE_VIEW_TYPE_2D};

    /// Additional usage flags beyond what the graph infers from pass
    /// declarations. Usually left 0 — the compile step ORs in flags from
    /// every declared gpu::Access against this resource.
    VkImageUsageFlags extra_usage{0};

    /// Persistent resources survive across frames (shadow atlas, DDGI
    /// atlases, Hi-Z history, TAA history). Non-persistent resources are
    /// candidates for transient aliasing.
    bool persistent{false};
};

struct BufferDesc {
    VkDeviceSize size{0};
    VkBufferUsageFlags extra_usage{0};
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

/// Which queue the graph should schedule a pass on.
enum class QueueType : uint8_t {
    graphics,
    async_compute,
};

} // namespace fjell
