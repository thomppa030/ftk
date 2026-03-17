#pragma once

#include "renderer/uniform_types.hpp"

#include <cstdint>

namespace fjell {

struct GpuMaterial;

class Material {
public:
    Material(uint32_t material_id, uint32_t base_color_idx, uint32_t normal_idx,
             uint32_t metallic_roughness_idx, uint32_t emissive_idx,
             PbrParams params = {});

    [[nodiscard]] const PbrParams& params() const { return params_; }
    [[nodiscard]] uint32_t material_id() const { return material_id_; }
    [[nodiscard]] GpuMaterial gpu_material() const;

private:
    uint32_t material_id_;
    uint32_t base_color_idx_;
    uint32_t normal_idx_;
    uint32_t metallic_roughness_idx_;
    uint32_t emissive_idx_;
    PbrParams params_;
};

} // namespace fjell
