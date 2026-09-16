#pragma once

#include "renderer/resources/fjwater.hpp"

#include <glm/glm.hpp>

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

/// The wave field, as one uniform buffer.
///
/// Too large for push constants: sixteen trains is 512 bytes against the
/// 128 Vulkan guarantees, so this goes in a buffer on the water's own
/// descriptor set rather than in the push block the way terrain's constants
/// do.
///
/// Only the runtime half of `FjWaterData` is here. The generator settings are
/// how a sea was authored, not what it is, and never reach the GPU.
struct GpuWaterField {
    GpuWaterWave waves[MAX_WATER_WAVES];
    std::uint32_t wave_count{0};
    float amplitude_scale{1.0f};
    float speed_scale{1.0f};
    float _pad0{0.0f};
};
static_assert(sizeof(GpuWaterField) == MAX_WATER_WAVES * 32 + 16,
              "must match FjellWaterField in GLSL");

/// Pack a loaded `.fjwater` into the shader's shape, dropping trains past
/// MAX_WATER_WAVES and any that cannot move the surface — the same two rules
/// the CPU evaluator applies, so both see the same sea.
[[nodiscard]] GpuWaterField to_gpu_field(const FjWaterData& water);

} // namespace fjell
