#pragma once

#include "core/handle.hpp"
#include "gpu/buffer.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace fjell::gpu {

class Device;
struct AccelerationStructureTag;

/// A ray tracing acceleration structure, by handle: a bottom level over
/// triangles, or a top level over instances of bottom levels. Made by
/// `Device::create`, held by `Owned<AccelerationStructure>`, built by
/// `CommandList::build` and traced by ray queries through
/// `gpu::acceleration()`.
using AccelerationStructure = Handle<AccelerationStructureTag>;

/// Hands a structure back to its device, which destroys it once the GPU is
/// done with it. `Owned<AccelerationStructure>` calls it.
void release(Device& device, AccelerationStructure structure);

/// Triangles a bottom level is built over: `vertex_count` positions, each
/// three 32-bit floats at the start of a vertex `vertex_stride` bytes long,
/// from the start of `vertices`, indexed by 32-bit `indices`, three to a
/// triangle. Both buffers are made with `BufferUse::acceleration_input`, and
/// a build reads them as `Access::acceleration_build_input`.
struct Triangles {
    BufferRange vertices{};
    uint32_t vertex_stride{0};
    uint32_t vertex_count{0};
    BufferRange indices{};
    uint32_t triangle_count{0};
};

/// How a structure is used, which the device weighs building it against
/// tracing it by.
enum class AccelerationUse : uint8_t {
    /// Built once and traced for many frames: a mesh.
    traced,
    /// Built anew every frame: a scene's instances.
    rebuilt,
};

/// What `Device::create` makes a structure for, which sizes it: a bottom
/// level for triangles like `triangles` (their counts matter, not where
/// they are), or, with `instances` above 0, a top level holding at most that
/// many.
///
/// @code
/// auto blas = device.create(gpu::AccelerationStructureDesc{
///     .triangles = {.vertex_stride = sizeof(Vertex), .vertex_count = 24,
///                   .triangle_count = 12},
///     .name = "crate",
/// });
/// @endcode
struct AccelerationStructureDesc {
    Triangles triangles{};
    uint32_t instances{0};
    AccelerationUse use{AccelerationUse::traced};
    /// Shown by debuggers and validation messages; not kept.
    std::string_view name{};
};

/// One instance a top level holds: a bottom level, placed.
struct AccelerationInstance {
    /// Object to world: the three rows of a 3 × 4 matrix, whose last column
    /// is the translation.
    std::array<float, 12> transform{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    /// What a ray query reads back as the hit's custom index. 24 bits.
    uint32_t custom_index{0};
    /// Which rays see it: those whose cull mask shares a bit with it.
    uint8_t mask{0xFF};
    /// Whether a ray that culls back faces culls this instance's; otherwise
    /// both sides of its triangles are hit.
    bool cull_back_faces{false};
    AccelerationStructure structure{};
};

/// The widest custom index an instance holds: 24 bits on Vulkan and Metal.
inline constexpr uint32_t MAX_CUSTOM_INDEX = (1U << 24) - 1;

} // namespace fjell::gpu
