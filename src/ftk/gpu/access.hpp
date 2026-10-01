#pragma once

#include "ftk/gpu/flags.hpp"
#include "ftk/gpu/usage.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

    // Copies and clears
    copy_src,
    copy_dst,
    /// Written by `clear` (a texture) or `fill` (a buffer).
    clear,

    /// Shown in a window: what a swapchain image is left in to be presented,
    /// and what an acquired one waits for before it is drawn into again.
    present,
    /// Read by the CPU once the GPU is done: a readback's buffer.
    host_read,

    // Acceleration structures
    /// The structure a build writes.
    acceleration_build,
    /// What a build reads: the vertices, indices or instance records it is
    /// built from, and the bottom levels a top level's instances name.
    acceleration_build_input,
    /// Traced by ray queries in a compute shader.
    acceleration_trace_compute,
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
        case Access::storage_buffer_write_compute:
        case Access::storage_buffer_read_write_compute:
        case Access::copy_dst:
        case Access::clear:
        case Access::acceleration_build:
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
        case Access::storage_buffer_write_compute:
        case Access::copy_dst:
        case Access::clear:
        case Access::acceleration_build:
            return false;
        default:
            return true;
    }
}

/// Whether a texture can be used this way.
[[nodiscard]] constexpr bool applies_to_texture(Access a) noexcept {
    switch (a) {
        case Access::color_attachment:
        case Access::depth_attachment:
        case Access::depth_resolve:
        case Access::depth_attachment_read:
        case Access::depth_read_sampled:
        case Access::input_attachment:
        case Access::sampled_fragment:
        case Access::sampled_vertex:
        case Access::sampled_mesh:
        case Access::sampled_compute:
        case Access::storage_read_compute:
        case Access::storage_write_compute:
        case Access::storage_read_write_compute:
        case Access::copy_src:
        case Access::copy_dst:
        case Access::clear:
        case Access::present:
            return true;
        default:
            return false;
    }
}

/// Whether a buffer can be used this way.
[[nodiscard]] constexpr bool applies_to_buffer(Access a) noexcept {
    switch (a) {
        case Access::uniform_read:
        case Access::storage_buffer_read_compute:
        case Access::storage_buffer_read_vertex:
        case Access::storage_buffer_read_fragment:
        case Access::storage_buffer_read_mesh:
        case Access::indirect_read:
        case Access::index_read:
        case Access::vertex_read:
        case Access::storage_buffer_write_compute:
        case Access::storage_buffer_read_write_compute:
        case Access::copy_src:
        case Access::copy_dst:
        case Access::clear:
        case Access::host_read:
        case Access::acceleration_build_input:
            return true;
        default:
            return false;
    }
}

/// Whether an acceleration structure can be used this way.
[[nodiscard]] constexpr bool applies_to_acceleration(Access a) noexcept {
    switch (a) {
        case Access::acceleration_build:
        case Access::acceleration_build_input:
        case Access::acceleration_trace_compute:
            return true;
        default:
            return false;
    }
}

/// The first of `accesses` a resource cannot be used as, by `applies`
/// (`applies_to_texture`, `applies_to_buffer` or `applies_to_acceleration`):
/// one it has no scope for, which a barrier on it would order nothing by.
/// Nothing when every one applies.
[[nodiscard]] constexpr std::optional<Access> first_misapplied(AccessSet accesses,
                                                               bool (*applies)(Access) noexcept) noexcept {
    std::optional<Access> found;
    accesses.for_each([&](Access a) {
        if (!found && !applies(a)) found = a;
    });
    return found;
}

/// The access's name as `Access` spells it, for reports.
[[nodiscard]] constexpr std::string_view access_name(Access a) noexcept {
    switch (a) {
        case Access::color_attachment: return "color_attachment";
        case Access::depth_attachment: return "depth_attachment";
        case Access::depth_resolve: return "depth_resolve";
        case Access::depth_attachment_read: return "depth_attachment_read";
        case Access::depth_read_sampled: return "depth_read_sampled";
        case Access::input_attachment: return "input_attachment";
        case Access::sampled_fragment: return "sampled_fragment";
        case Access::sampled_vertex: return "sampled_vertex";
        case Access::sampled_mesh: return "sampled_mesh";
        case Access::sampled_compute: return "sampled_compute";
        case Access::storage_read_compute: return "storage_read_compute";
        case Access::storage_write_compute: return "storage_write_compute";
        case Access::storage_read_write_compute: return "storage_read_write_compute";
        case Access::uniform_read: return "uniform_read";
        case Access::storage_buffer_read_compute: return "storage_buffer_read_compute";
        case Access::storage_buffer_read_vertex: return "storage_buffer_read_vertex";
        case Access::storage_buffer_read_fragment: return "storage_buffer_read_fragment";
        case Access::storage_buffer_read_mesh: return "storage_buffer_read_mesh";
        case Access::indirect_read: return "indirect_read";
        case Access::index_read: return "index_read";
        case Access::vertex_read: return "vertex_read";
        case Access::storage_buffer_write_compute: return "storage_buffer_write_compute";
        case Access::storage_buffer_read_write_compute: return "storage_buffer_read_write_compute";
        case Access::copy_src: return "copy_src";
        case Access::copy_dst: return "copy_dst";
        case Access::clear: return "clear";
        case Access::present: return "present";
        case Access::host_read: return "host_read";
        case Access::acceleration_build: return "acceleration_build";
        case Access::acceleration_build_input: return "acceleration_build_input";
        case Access::acceleration_trace_compute: return "acceleration_trace_compute";
    }
    return {};
}

/// What a texture must be made able to do to be used this way: nothing for
/// an access that does not apply to textures, nothing for a copy or a clear,
/// which every texture allows, and nothing for present, which only a
/// swapchain's images are, made for it.
[[nodiscard]] constexpr TextureUses texture_use(Access a) noexcept {
    switch (a) {
        case Access::color_attachment:
            return TextureUse::color_target;
        case Access::depth_attachment:
        case Access::depth_resolve:
        case Access::depth_attachment_read:
        case Access::input_attachment:
            return TextureUse::depth_target;
        case Access::depth_read_sampled:
            return TextureUse::depth_target | TextureUse::sampled;
        case Access::sampled_fragment:
        case Access::sampled_vertex:
        case Access::sampled_mesh:
        case Access::sampled_compute:
            return TextureUse::sampled;
        case Access::storage_read_compute:
        case Access::storage_write_compute:
        case Access::storage_read_write_compute:
            return TextureUse::storage;
        default:
            return {};
    }
}

/// How the backend names the state it keeps a texture in for `accesses` (a
/// depth texture when `depth`), for traces: on Vulkan, the image layout.
[[nodiscard]] std::string state_name(AccessSet accesses, bool depth);

} // namespace fjell::gpu
