#pragma once

#include "renderer/uniform_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fjell {

struct GpuMaterial;

class Material {
public:
    Material(uint32_t material_id, uint32_t base_color_idx, uint32_t normal_idx,
             uint32_t metallic_roughness_idx, uint32_t emissive_idx,
             PbrParams params = {});

    [[nodiscard]] const PbrParams& params() const { return params_; }
    [[nodiscard]] uint32_t material_id() const { return material_id_; }
    [[nodiscard]] uint32_t pipeline_id() const { return pipeline_id_; }
    [[nodiscard]] GpuMaterial gpu_material() const;

    void set_pipeline_id(uint32_t id) { pipeline_id_ = id; }

    // Custom texture slots (indices 4-7)
    void set_custom_texture(uint32_t slot, uint32_t bindless_idx);

    // Custom params SSBO offset/count
    void set_custom_params(uint32_t offset, uint32_t count) {
        custom_param_offset_ = offset;
        custom_param_count_ = count;
    }

    [[nodiscard]] uint32_t custom_param_offset() const { return custom_param_offset_; }
    [[nodiscard]] uint32_t custom_param_count() const { return custom_param_count_; }

    // Shader path (for .fjmat round-tripping)
    void set_shader_path(const std::string& path) { shader_path_ = path; }
    [[nodiscard]] const std::string& shader_path() const { return shader_path_; }

private:
    uint32_t material_id_;
    uint32_t pipeline_id_{0};
    uint32_t base_color_idx_;
    uint32_t normal_idx_;
    uint32_t metallic_roughness_idx_;
    uint32_t emissive_idx_;
    uint32_t custom_texture_indices_[4]{}; // slots 4-7
    uint32_t custom_param_offset_{0};
    uint32_t custom_param_count_{0};
    PbrParams params_;
    std::string shader_path_{"shaders/standard.fjsl"};
};

} // namespace fjell
