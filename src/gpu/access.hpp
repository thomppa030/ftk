#pragma once

#include "gpu/flags.hpp"

#include <cstdint>

namespace fjell::gpu {

/// How a pass or a command uses a resource: the one word synchronisation is
/// derived from. The Vulkan backend turns it into a layout, pipeline stages
/// and access masks; Metal into stage barriers and fences. In the frame graph
/// it also says whether a declared use is a read, a write or both.
enum class Access : uint8_t {
    // Graphics attachment uses (write)
    color_attachment,
    depth_attachment,
    /// The single-sample image a multisampled depth attachment resolves
    /// into when rendering ends. The resolve writes it at the colour
    /// attachment output stage, not in the depth tests.
    depth_resolve,

    // Graphics attachment uses (read-only)
    depth_attachment_read,
    /// Depth bound read-only for the depth test while the fragment shader
    /// also samples it: one layout serves both.
    depth_read_sampled,
    input_attachment,

    // Shader reads
    sampled_fragment,
    sampled_vertex,
    /// Sampled by a task or mesh shader.
    sampled_mesh,
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
    /// Read by a task or mesh shader.
    storage_buffer_read_mesh,
    indirect_read,
    index_read,
    vertex_read,

    // Buffers (writes)
    storage_buffer_write_compute,
    storage_buffer_read_write_compute,

    // Copies
    copy_src,
    copy_dst,
};

template <>
inline constexpr bool is_flag_enum<Access> = true;

/// A set of `Access`es.
using AccessSet = Flags<Access>;

/// Whether the access changes what the resource holds.
[[nodiscard]] constexpr bool access_is_write(Access a) noexcept {
    switch (a) {
        case Access::color_attachment:
        case Access::depth_attachment:
        case Access::depth_resolve:
        case Access::storage_write_compute:
        case Access::storage_read_write_compute:
        case Access::storage_write_raytracing:
        case Access::storage_buffer_write_compute:
        case Access::storage_buffer_read_write_compute:
        case Access::copy_dst:
            return true;
        default:
            return false;
    }
}

/// Whether the access depends on what the resource already holds. An
/// attachment counts: a pass loads what earlier passes drew before it draws
/// over them, and two writers of one image have to be ordered either way,
/// so the earlier one is a producer the later one reads.
[[nodiscard]] constexpr bool access_is_read(Access a) noexcept {
    switch (a) {
        case Access::depth_resolve:
        case Access::storage_write_compute:
        case Access::storage_write_raytracing:
        case Access::storage_buffer_write_compute:
        case Access::copy_dst:
            return false;
        default:
            return true;
    }
}

} // namespace fjell::gpu
