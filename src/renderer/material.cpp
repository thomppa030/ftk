#include "renderer/material.hpp"
#include "renderer/bindless.hpp"

namespace fjell {

Material::Material(uint32_t material_id, uint32_t base_color_idx, uint32_t normal_idx,
                   uint32_t metallic_roughness_idx, uint32_t emissive_idx,
                   PbrParams params)
    : material_id_{material_id}
    , base_color_idx_{base_color_idx}
    , normal_idx_{normal_idx}
    , metallic_roughness_idx_{metallic_roughness_idx}
    , emissive_idx_{emissive_idx}
    , params_{params} {}

GpuMaterial Material::gpu_material() const {
    GpuMaterial m{};
    m.base_color_idx = base_color_idx_;
    m.normal_idx = normal_idx_;
    m.metallic_roughness_idx = metallic_roughness_idx_;
    m.emissive_idx = emissive_idx_;
    m.base_color_factor[0] = params_.base_color_factor.x;
    m.base_color_factor[1] = params_.base_color_factor.y;
    m.base_color_factor[2] = params_.base_color_factor.z;
    m.base_color_factor[3] = params_.base_color_factor.w;
    m.metallic_factor = params_.metallic_factor;
    m.roughness_factor = params_.roughness_factor;
    m.has_normal_map = params_.has_normal_map ? 1.0f : 0.0f;
    return m;
}

} // namespace fjell
