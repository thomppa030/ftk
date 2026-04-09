#include "renderer/resources/material.hpp"
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

    // Texture slots (standard PBR layout: 0-3)
    m.texture_indices[0] = base_color_idx_;
    m.texture_indices[1] = normal_idx_;
    m.texture_indices[2] = metallic_roughness_idx_;
    m.texture_indices[3] = emissive_idx_;

    // Custom texture slots (4-7)
    for (int i = 0; i < 4; ++i) {
        m.texture_indices[4 + i] = custom_texture_indices_[i];
    }

    // Base PBR params
    m.base_params[0] = params_.base_color_factor.x;
    m.base_params[1] = params_.base_color_factor.y;
    m.base_params[2] = params_.base_color_factor.z;
    m.base_params[3] = params_.base_color_factor.w;
    m.base_params[4] = params_.metallic_factor;
    m.base_params[5] = params_.roughness_factor;
    m.base_params[6] = params_.has_normal_map ? 1.0f : 0.0f;
    m.base_params[7] = params_.uv_scale.x;
    m.base_params[8] = params_.uv_scale.y;
    m.base_params[9] = params_.emission_color.x;
    m.base_params[10] = params_.emission_color.y;
    m.base_params[11] = params_.emission_color.z;

    // Custom params reference
    m.param_offset = custom_param_offset_;
    m.param_count = custom_param_count_;

    return m;
}

void Material::set_custom_texture(uint32_t slot, uint32_t bindless_idx) {
    if (slot >= 4 && slot <= 7) {
        custom_texture_indices_[slot - 4] = bindless_idx;
    }
}

} // namespace fjell
