// The ray tracing instance records (renderer/rt_instance.hpp): which rays
// stop at a surface, by how it is drawn, and the shaders' copy of the
// per-instance data and its constants (fjell_gpu_types.glsl) agreeing with
// the engine's.

#include "renderer/rt_instance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

namespace {

// A ray query's cull mask for rays that gather diffuse light, and for
// reflection rays, as the tracers write them.
constexpr uint32_t DIFFUSE_RAYS = fjell::RT_MASK_OPAQUE;
constexpr uint32_t REFLECTION_RAYS = fjell::RT_MASK_OPAQUE | fjell::RT_MASK_TRANSPARENT;

[[nodiscard]] bool stops(uint32_t ray_mask, uint32_t instance_mask) {
    return (ray_mask & instance_mask) != 0;
}

} // namespace

TEST_CASE("RT instance: diffuse rays pass through glass, reflections see it", "[rt_instance]") {
    const uint32_t opaque = fjell::rt_instance_mask(false);
    const uint32_t glass = fjell::rt_instance_mask(true);

    CHECK(stops(DIFFUSE_RAYS, opaque));
    CHECK_FALSE(stops(DIFFUSE_RAYS, glass));
    CHECK(stops(REFLECTION_RAYS, opaque));
    CHECK(stops(REFLECTION_RAYS, glass));
}

TEST_CASE("RT instance: every mask fits the instance's eight bits and is hit by something",
          "[rt_instance]") {
    // VkAccelerationStructureInstanceKHR::mask is 8 bits, and the particle
    // shader shifts it into the top byte; a zero mask is never hit.
    for (const bool transparent : {false, true}) {
        const uint32_t mask = fjell::rt_instance_mask(transparent);
        CHECK(mask != 0U);
        CHECK(mask <= 0xFFU);
    }
    CHECK((fjell::RT_MASK_OPAQUE & fjell::RT_MASK_TRANSPARENT) == 0U);
}

TEST_CASE("RT instance: a TLAS is reused only for the very same instances", "[rt_instance]") {
    fjell::RtInstance built{};
    built.transform[3] = glm::vec4(4.0f, 0.0f, -2.0f, 1.0f);
    built.blas = fjell::gpu::AccelerationStructure::make(1, 1);
    built.first_index = 30;
    built.vertex_offset = 12;
    built.material_id = 7;
    built.mask = fjell::RT_MASK_OPAQUE;

    fjell::RtInstance now = built;
    CHECK(now == built);

    // Each field on its own is enough to call for a new build.
    now.transform[3].x += 0.001f;
    CHECK_FALSE(now == built);
    now = built;
    now.blas = fjell::gpu::AccelerationStructure::make(2, 1);
    CHECK_FALSE(now == built);
    // The same slot made anew: another structure.
    now.blas = fjell::gpu::AccelerationStructure::make(1, 2);
    CHECK_FALSE(now == built);
    now = built;
    now.first_index = 31;
    CHECK_FALSE(now == built);
    now = built;
    now.vertex_offset = 13;
    CHECK_FALSE(now == built);
    now = built;
    now.material_id = 8;
    CHECK_FALSE(now == built);
    now = built;
    now.mask = fjell::RT_MASK_TRANSPARENT;
    CHECK_FALSE(now == built);
    now = built;
    now.two_sided = true;
    CHECK_FALSE(now == built);
}

// The engine's values, read here before the shader's macros of the same
// names take those names over for the rest of the file.
namespace engine_rt {
constexpr uint32_t two_sided = fjell::RT_INSTANCE_TWO_SIDED;
constexpr uint32_t mask_opaque = fjell::RT_MASK_OPAQUE;
constexpr uint32_t mask_transparent = fjell::RT_MASK_TRANSPARENT;
constexpr std::size_t instance_size = sizeof(fjell::GpuRtInstance);
constexpr std::size_t flags_offset = offsetof(fjell::GpuRtInstance, flags);
} // namespace engine_rt

#include "glsl_shim.hpp"

namespace rt_instance_shim {
#include "../shaders/include/fjell_gpu_types.glsl"
} // namespace rt_instance_shim

TEST_CASE("RT instance: the shaders read the engine's instance data and constants",
          "[rt_instance]") {
    STATIC_REQUIRE(RT_INSTANCE_TWO_SIDED == engine_rt::two_sided);
    STATIC_REQUIRE(RT_MASK_OPAQUE == engine_rt::mask_opaque);
    STATIC_REQUIRE(RT_MASK_TRANSPARENT == engine_rt::mask_transparent);
    STATIC_REQUIRE(sizeof(rt_instance_shim::RtInstanceData) == engine_rt::instance_size);
    STATIC_REQUIRE(offsetof(rt_instance_shim::RtInstanceData, flags) == engine_rt::flags_offset);
}
