#pragma once

#include "ftk/gpu/acceleration.hpp"

#include <glm/mat4x4.hpp>

#include <cstdint>

namespace fjell {

/// Per-instance data the ray tracers read for a hit, by the instance's index
/// in the TLAS: the material it carries and where its triangles start.
/// RtInstanceData in fjell_gpu_types.glsl.
struct GpuRtInstance {
    uint32_t material_id;
    uint32_t first_index;    // into the mega-buffer's index buffer
    uint32_t vertex_offset;  // into the mega-buffer's vertex buffer
    uint32_t flags;          // RT_INSTANCE_* bits
};

/// GpuRtInstance::flags: the instance's material draws both sides of its
/// surfaces, so a ray reaching either side has hit the surface's front.
/// RT_INSTANCE_TWO_SIDED in fjell_gpu_types.glsl.
inline constexpr uint32_t RT_INSTANCE_TWO_SIDED = 1U;

/// Which rays stop at an instance. The instance's mask holds the bit of its
/// kind and a ray query's cull mask names the kinds that ray can hit.
/// RT_MASK_* in fjell_gpu_types.glsl.
///
/// Surfaces that stop light: everything drawn opaque, and terrain.
inline constexpr uint32_t RT_MASK_OPAQUE = 0x01U;
/// Surfaces drawn blended, glass above all. Rays gathering diffuse light
/// (DDGI probes, SS-GI) and shadow tests leave them out, so light passes
/// through glass; reflection rays stop at them, so glass shows in a mirror.
inline constexpr uint32_t RT_MASK_TRANSPARENT = 0x02U;

/// The mask of a surface by how it is drawn.
[[nodiscard]] constexpr uint32_t rt_instance_mask(bool transparent) {
    return transparent ? RT_MASK_TRANSPARENT : RT_MASK_OPAQUE;
}

/// One TLAS instance as the CPU lists it. Everything whose count is known on
/// the CPU (meshes, terrain, whatever comes next) reduces to this, so adding
/// a source means listing these rather than extending the build.
///
/// Particles are deliberately not expressible here: their instances are
/// written by a compute shader into a region reserved from their upper bound,
/// because the count is not known until the GPU has culled them.
struct RtInstance {
    glm::mat4 transform{1.0F};
    /// The bottom level: a structure made anew is another handle, so a list
    /// naming it differs from one built before.
    gpu::AccelerationStructure blas{};
    uint32_t first_index{0};
    uint32_t vertex_offset{0};
    uint32_t material_id{0};
    /// RT_MASK_* of the surface's kind.
    uint32_t mask{RT_MASK_OPAQUE};
    /// The material draws both sides (its shader culls no faces, or the
    /// front ones), so neither side is a back face to a ray.
    bool two_sided{false};

    /// Every field alike: a TLAS built from one list of instances holds for
    /// an equal list.
    bool operator==(const RtInstance&) const = default;
};

} // namespace fjell
