#include "renderer/resources/water_gpu.hpp"

#include "renderer/resources/fjterrain.hpp"
#include "renderer/resources/water_surface.hpp"

#include <algorithm>

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
    if (water.shore.has_value() && water.shore->terrain != nullptr) {
        const water::ShoreBed& bed = *water.shore;
        const FjTerrainHeader& header = bed.terrain->header;
        GpuWaterShore& shore = field.shore;
        shore.bed_min_corner = bed.min_corner;
        shore.bed_size = header.world_size;
        shore.bed_samples = header.sample_count;
        shore.bed_height_scale = header.height_scale;
        shore.bed_height_offset = header.height_offset;
        shore.ground_y = bed.ground_y;
        shore.surface_y = bed.surface_y;
        if (water.packed_trains.has_value()) {
            const water::ShoalingSum& sum = water.packed_trains->shoaling;
            shore.trains = {sum.k, sum.amplitude};
        }
        if (water.simulated.has_value()) {
            const water::ShoalingSum& sum = water.simulated->shoaling;
            shore.simulated = {sum.k, sum.amplitude};
        }
        shore.present = 1;
        if (bed.exposure != nullptr) {
            shore.exposure_min_corner = bed.exposure->min_corner;
            shore.exposure_size = bed.exposure->size;
            shore.exposure_samples = bed.exposure->samples;
        }
        if (bed.warp != nullptr) {
            shore.warp_min_corner = bed.warp->min_corner;
            shore.warp_size = bed.warp->size;
            shore.warp_samples = bed.warp->samples;
        }
    }
    return field;
}

GpuWakeSource to_gpu_wake(const water::WakeSource& s) {
    GpuWakeSource out;
    out.position = glm::vec4(s.position, s.velocity);
    out.bounds = glm::vec4(s.bounds_min, s.bounds_max);
    out.ring = glm::vec4(s.k, s.ring_speed, s.width, s.decay);
    out.body = glm::vec4(s.radius, s.time, 0.0f, 0.0f);
    out.churn = glm::vec4(s.foam_scale, s.foam_decay, 0.0f, 0.0f);
    const std::size_t count = std::min<std::size_t>(s.stamps.size(), water::MAX_WAKE_STAMPS);
    out.counts = glm::uvec4(static_cast<std::uint32_t>(count), s.displaces ? 1u : 0u, 0u, 0u);
    for (std::size_t i = 0; i < count; ++i) {
        const water::WakeStamp& stamp = s.stamps[i];
        out.stamps[i] = glm::vec4(stamp.position, stamp.time, stamp.amplitude);
    }
    return out;
}

} // namespace fjell
