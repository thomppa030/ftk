#include "ui/material_params_widget.hpp"
#include "core/string_utils.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/uniform_types.hpp"

#include <imgui.h>

namespace fjell {

bool draw_material_params(const ShaderMetadata& meta,
                           BindlessTextures& bindless,
                           Material& material) {
    bool changed = false;

    // Base PBR params (base_params[4]/[5] — see GpuMaterial in bindless.hpp).
    // These exist on every material regardless of shader — including custom
    // FJSL shaders like solid_lit.fjsl that only ever declare their own
    // uniforms (tint, etc.) and never reference ROUGHNESS/METALLIC in
    // fragment(), where they still fall through to these factors. Shown
    // above the custom-uniform loop below so a shader with zero custom
    // uniforms still gets an editable material.
    {
        GpuMaterial base = bindless.material_at(material.material_id());
        float metallic = base.base_params[4];
        float roughness = base.base_params[5];
        bool base_changed = false;

        ImGui::TextDisabled("Base PBR");
        ImGui::PushID("__metallic_factor");
        ImGui::Text("Metallic");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##val", &metallic, 0.0f, 1.0f)) base_changed = true;
        ImGui::PopID();
        ImGui::PushID("__roughness_factor");
        ImGui::Text("Roughness");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##val", &roughness, 0.0f, 1.0f)) base_changed = true;
        ImGui::PopID();
        ImGui::Separator();

        if (base_changed) {
            base.base_params[4] = metallic;
            base.base_params[5] = roughness;
            bindless.update_material(material.material_id(), base);
            changed = true;
        }
    }

    uint32_t param_count = material.custom_param_count();
    if (param_count == 0) return changed;

    const auto& gpu = bindless.material_at(material.material_id());

    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;

        uint32_t fcount = uniform_float_count(u.type);
        if (u.slot + fcount > param_count) continue;

        uint32_t abs_offset = gpu.param_offset + u.slot;

        ImGui::PushID(u.name.c_str());
        ImGui::Text("%s", display_name(u.name).c_str());
        ImGui::SetNextItemWidth(-1);

        // Read current values from CPU-side params buffer
        float temp[4]{};
        for (uint32_t i = 0; i < fcount; ++i) {
            temp[i] = bindless.read_param(abs_offset + i);
        }

        switch (u.type) {
            case UniformType::Float:
                if (u.hint == UniformHint::None && u.range.max > u.range.min) {
                    if (ImGui::SliderFloat("##val", temp, u.range.min, u.range.max)) {
                        bindless.write_params(abs_offset, temp, 1);
                        changed = true;
                    }
                } else {
                    if (ImGui::DragFloat("##val", temp, 0.01f)) {
                        bindless.write_params(abs_offset, temp, 1);
                        changed = true;
                    }
                }
                break;

            case UniformType::Vec2:
                if (ImGui::DragFloat2("##val", temp, 0.01f)) {
                    bindless.write_params(abs_offset, temp, 2);
                    changed = true;
                }
                break;

            case UniformType::Vec3:
                if (u.hint == UniformHint::Color) {
                    if (ImGui::ColorEdit3("##val", temp)) {
                        bindless.write_params(abs_offset, temp, 3);
                        changed = true;
                    }
                } else {
                    if (ImGui::DragFloat3("##val", temp, 0.01f)) {
                        bindless.write_params(abs_offset, temp, 3);
                        changed = true;
                    }
                }
                break;

            case UniformType::Vec4:
                if (u.hint == UniformHint::Color) {
                    if (ImGui::ColorEdit4("##val", temp)) {
                        bindless.write_params(abs_offset, temp, 4);
                        changed = true;
                    }
                } else {
                    if (ImGui::DragFloat4("##val", temp, 0.01f)) {
                        bindless.write_params(abs_offset, temp, 4);
                        changed = true;
                    }
                }
                break;

            default:
                break;
        }

        ImGui::PopID();
    }

    // Recompute emission_color in base_params[9..11] for RT GI
    if (changed) {
        float emission_strength = 0.0f;
        glm::vec3 glow_color{0.0f};
        bool has_glow_color = false;

        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) continue;
            uint32_t fcount = uniform_float_count(u.type);
            if (u.slot + fcount > param_count) continue;

            uint32_t abs = gpu.param_offset + u.slot;

            if ((u.name == "emission_strength" || u.name == "glow_strength") && fcount >= 1) {
                emission_strength = bindless.read_param(abs);
            } else if ((u.name == "glow_color" || u.name == "emission_color") && fcount >= 3) {
                glow_color = {bindless.read_param(abs), bindless.read_param(abs + 1), bindless.read_param(abs + 2)};
                has_glow_color = true;
            }
        }

        glm::vec3 emission{0.0f};
        if (has_glow_color && emission_strength > 0.0f) {
            emission = glow_color * emission_strength;
        } else if (has_glow_color) {
            emission = glow_color;
        } else if (emission_strength > 0.0f) {
            emission = glm::vec3(emission_strength);
        }

        GpuMaterial updated = bindless.material_at(material.material_id());
        updated.base_params[9] = emission.x;
        updated.base_params[10] = emission.y;
        updated.base_params[11] = emission.z;
        bindless.update_material(material.material_id(), updated);
    }

    return changed;
}

} // namespace fjell
