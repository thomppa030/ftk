#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace fjell {

/// How a pass uses a resource. Drives layout, pipeline stage, and access
/// flags for automatic barrier insertion, and declares the edge type in
/// the DAG (read / write / read-write).
enum class ResourceAccess : uint8_t {
    // Graphics attachment uses (write)
    color_attachment,
    depth_attachment,

    // Graphics attachment uses (read-only)
    depth_attachment_read,
    input_attachment,

    // Shader reads
    sampled_fragment,
    sampled_vertex,
    sampled_compute,

    // Storage image
    storage_read_compute,
    storage_write_compute,
    storage_read_write_compute,

    // Ray tracing shaders (vkCmdTraceRaysKHR). A distinct pipeline stage
    // from compute: a barrier addressed to the compute stage does not
    // order a trace.
    sampled_raytracing,
    storage_write_raytracing,

    // Buffers (reads)
    uniform_read,
    storage_buffer_read_compute,
    storage_buffer_read_vertex,
    storage_buffer_read_fragment,
    indirect_read,
    index_read,
    vertex_read,

    // Buffers (writes)
    storage_buffer_write_compute,
    storage_buffer_read_write_compute,

    // Copies
    transfer_src,
    transfer_dst,
};

[[nodiscard]] constexpr bool access_is_write(ResourceAccess a) noexcept {
    switch (a) {
        case ResourceAccess::color_attachment:
        case ResourceAccess::depth_attachment:
        case ResourceAccess::storage_write_compute:
        case ResourceAccess::storage_read_write_compute:
        case ResourceAccess::storage_write_raytracing:
        case ResourceAccess::storage_buffer_write_compute:
        case ResourceAccess::storage_buffer_read_write_compute:
        case ResourceAccess::transfer_dst:
            return true;
        default:
            return false;
    }
}

/// Whether the access depends on what the resource already holds. An
/// attachment counts: a pass loads what earlier passes drew before it draws
/// over them, and two writers of one image have to be ordered either way,
/// so the earlier one is a producer the later one reads.
[[nodiscard]] constexpr bool access_is_read(ResourceAccess a) noexcept {
    switch (a) {
        case ResourceAccess::storage_write_compute:
        case ResourceAccess::storage_write_raytracing:
        case ResourceAccess::storage_buffer_write_compute:
        case ResourceAccess::transfer_dst:
            return false;
        default:
            return true;
    }
}

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
    /// every declared ResourceAccess against this resource.
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
