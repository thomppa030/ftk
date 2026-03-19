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

    // Texture slots (standard PBR layout)
    m.texture_indices[0] = base_color_idx_;
    m.texture_indices[1] = normal_idx_;
    m.texture_indices[2] = metallic_roughness_idx_;
    m.texture_indices[3] = emissive_idx_;

    // Param slots (standard PBR layout)
    m.params[0] = params_.base_color_factor.x;
    m.params[1] = params_.base_color_factor.y;
    m.params[2] = params_.base_color_factor.z;
    m.params[3] = params_.base_color_factor.w;
    m.params[4] = params_.metallic_factor;
    m.params[5] = params_.roughness_factor;
    m.params[6] = params_.has_normal_map ? 1.0f : 0.0f;

    return m;
}

} // namespace fjell
