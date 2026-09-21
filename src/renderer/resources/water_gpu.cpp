#include "renderer/resources/water_gpu.hpp"

#include "renderer/resources/water_surface.hpp"

namespace fjell {

GpuWaterField to_gpu_field(const FjWaterData& water) {
    GpuWaterField field;
    water::for_each_summed_train(water, [&](std::uint32_t slot,
                                            const GerstnerContributor& contributor,
                                            const WaterWave& wave) {
        // A contributor none of whose trains move the surface is never
        // visited, and its slot stays the empty block it started as.
        field.gerstner_count = slot + 1;

        auto& block = field.gerstner[slot];
        block.amplitude_scale = contributor.amplitude_scale;
        block.speed_scale = contributor.speed_scale;
        block.steepness_headroom = contributor.steepness_headroom;

        auto& out = block.waves[block.wave_count++];
        out.direction = wave.direction;
        out.wavelength = wave.wavelength;
        out.amplitude = wave.amplitude;
        out.steepness = wave.steepness;
        out.speed = wave.speed;
    });
    if (const auto* fft = water::simulated_sea(water)) {
        field.simulated_patch_size = fft->patch_size;
    }
    return field;
}

} // namespace fjell
