#include "renderer/resources/water_gpu.hpp"

#include <algorithm>
#include <variant>

namespace fjell {

GpuWaterField to_gpu_field(const FjWaterData& water) {
    GpuWaterField field;
    const GerstnerContributor* gerstner = nullptr;
    for (const auto& contributor : water.contributors) {
        gerstner = std::get_if<GerstnerContributor>(&contributor);
        if (gerstner != nullptr) break;
    }
    if (gerstner == nullptr) return field;

    field.amplitude_scale = gerstner->amplitude_scale;
    field.speed_scale = gerstner->speed_scale;
    field.steepness_headroom = gerstner->steepness_headroom;

    std::uint32_t count = 0;
    for (const auto& wave : gerstner->waves) {
        if (count >= MAX_WATER_WAVES) break;
        // A train with no direction or no length contributes nothing, and
        // both evaluators already skip it. Dropping it here keeps the shader
        // from spending an iteration deciding the same thing every vertex.
        if (glm::length(wave.direction) < 1e-6f || wave.wavelength < 1e-4f) continue;

        auto& out = field.waves[count];
        out.direction = wave.direction;
        out.wavelength = wave.wavelength;
        out.amplitude = wave.amplitude;
        out.steepness = wave.steepness;
        out.speed = wave.speed;
        ++count;
    }
    field.wave_count = count;
    return field;
}

} // namespace fjell
