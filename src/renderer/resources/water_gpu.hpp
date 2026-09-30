#pragma once

#include "renderer/resources/fjwater.hpp"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace fjell {

/// One wave train as the shader reads it.
///
/// Mirrors `FjellWaterWave` in shaders/include/fjell_water_waves.glsl. The
/// padding is not decoration: std140 rounds a struct up to its largest
/// member's alignment, so the vec2 makes this 8-byte aligned and the two
/// floats bring it to a size both sides agree on without either having to
/// guess at a stride. `test_fjwater.cpp` compiles the GLSL and checks the
/// layouts against each other rather than trusting that they match.
struct GpuWaterWave {
    glm::vec2 direction{1.0f, 0.0f};
    float wavelength{20.0f};
    float amplitude{0.5f};
    float steepness{0.5f};
    float speed{1.0f};
    float _pad0{0.0f};
    float _pad1{0.0f};
};
static_assert(sizeof(GpuWaterWave) == 32, "must match FjellWaterWave in GLSL");

/// One Gerstner contributor as the shader reads it: its trains and the scales
/// they move under. Mirrors `FjellGerstnerContributor`.
///
/// Only the runtime half of `GerstnerContributor` is here. The generator
/// settings are how it was authored and the wind response how it was derived,
/// not what it is, and neither reaches the GPU.
struct GpuGerstnerContributor {
    GpuWaterWave waves[MAX_WATER_WAVES];
    std::uint32_t wave_count{0};
    float amplitude_scale{1.0f};
    float speed_scale{1.0f};
    float steepness_headroom{1.0f};
};
static_assert(sizeof(GpuGerstnerContributor) == MAX_WATER_WAVES * 32 + 16,
              "must match FjellGerstnerContributor in GLSL");

/// The ground under the water as the shader reads it: where the terrain's
/// heights stand, and what each sum of the sea shoals by. Mirrors
/// `FjellWaterShore`. The heights are the terrain's own texture, bound
/// beside the field.
struct GpuWaterShore {
    glm::vec2 bed_min_corner{0.0f};
    float bed_size{0.0f};
    std::uint32_t bed_samples{0};
    float bed_height_scale{0.0f};
    float bed_height_offset{0.0f};
    float ground_y{0.0f};
    float surface_y{0.0f};
    /// Dominant wavenumber and significant amplitude, of the Gerstner sum and
    /// of the simulated sea.
    glm::vec2 trains{0.0f};
    glm::vec2 simulated{0.0f};
    /// 0 is open water everywhere, and nothing above is read.
    std::uint32_t present{0};
    std::uint32_t _pad0{0};
    std::uint32_t _pad1{0};
    std::uint32_t _pad2{0};
};
static_assert(sizeof(GpuWaterShore) == 64, "must match FjellWaterShore in GLSL");

/// The wave field, as one uniform buffer. Mirrors `FjellWaterField`.
///
/// Too large for push constants: one contributor of sixteen trains is 528
/// bytes against the 128 Vulkan guarantees, so this goes in a buffer on the
/// water's own descriptor set rather than in the push block the way terrain's
/// constants do.
///
/// std140 rounds a struct up to a multiple of sixteen bytes, so the count is
/// padded out here to the size the shader will step over rather than leaving
/// the tail to chance. The sizes here say the two sides are the same length;
/// that the members sit at the same offsets is checked against the compiled
/// shader, and member by member in `test_fjwater.cpp`.
struct GpuWaterField {
    GpuGerstnerContributor gerstner[MAX_GERSTNER_CONTRIBUTORS];
    std::uint32_t gerstner_count{0};
    /// Metres the simulated sea's textures tile over, or 0 when the field has
    /// no simulated contributor and they are not to be sampled. The
    /// simulation itself reaches the shader as textures, not through here.
    float simulated_patch_size{0.0f};
    std::uint32_t _pad0{0};
    std::uint32_t _pad1{0};
    GpuWaterShore shore;
};
static_assert(sizeof(GpuWaterField)
                  == MAX_GERSTNER_CONTRIBUTORS * sizeof(GpuGerstnerContributor) + 16
                         + sizeof(GpuWaterShore),
              "must match FjellWaterField in GLSL");
// Where the compiled shader puts them, read out of its SPIR-V.
static_assert(offsetof(GpuGerstnerContributor, wave_count) == 512);
static_assert(offsetof(GpuGerstnerContributor, steepness_headroom) == 524);
static_assert(offsetof(GpuWaterField, gerstner_count) == 2112);
static_assert(offsetof(GpuWaterField, simulated_patch_size) == 2116);
static_assert(offsetof(GpuWaterField, shore) == 2128);
static_assert(offsetof(GpuWaterShore, trains) == 32);
static_assert(offsetof(GpuWaterShore, present) == 48);

/// One source and its trail as the shader reads it. Mirrors `FjellWakeSource`
/// in shaders/include/fjell_water_wake.glsl. Nothing but vec4s, so std430 and
/// this struct put every member at the same offset with no padding to guess
/// at; the sizes are pinned here and the offsets read out of the compiled
/// shader.
struct GpuWakeSource {
    glm::vec4 position{0.0f};   // xy world XZ when recorded, zw velocity
    glm::vec4 bounds{0.0f};     // xy min, zw max, world XZ
    glm::vec4 ring{0.0f};       // k, spreading speed, envelope width, decay time
    glm::vec4 body{0.0f};       // radius, time recorded, 0, 0
    glm::vec4 churn{0.0f};      // foam: amplitude scale, decay time, 0, 0
    glm::uvec4 counts{0};       // stamps, displaces, 0, 0
    glm::vec4 stamps[water::MAX_WAKE_STAMPS]{};   // xy world XZ, z time laid, w amplitude
};
static_assert(sizeof(GpuWakeSource) == (6 + water::MAX_WAKE_STAMPS) * 16,
              "must match FjellWakeSource in GLSL");
static_assert(offsetof(GpuWakeSource, counts) == 80);
static_assert(offsetof(GpuWakeSource, stamps) == 96);

/// Pack a wake source into the shader's shape.
[[nodiscard]] GpuWakeSource to_gpu_wake(const water::WakeSource& source);

/// Pack a field into the shader's shape: exactly the trains
/// `water::for_each_summed_train()` walks, which is what the CPU evaluator
/// sums, so both see the same sea.
[[nodiscard]] GpuWaterField to_gpu_field(const FjWaterData& water);

} // namespace fjell
