#include "ui/material_params_widget.hpp"
#include "core/string_utils.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_gi_params.hpp"
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

    // Re-derive what the GI paths read of this material from the live param
    // values, the same way the loader derives it from the asset, so an edit
    // here lights the scene exactly as saving and reloading would.
    if (changed) {
        auto gi = derive_material_gi_params(meta, [&](const FjslUniform& u) {
            std::vector<float> values;
            uint32_t fcount = uniform_float_count(u.type);
            if (u.slot + fcount > param_count) return values;
            values.reserve(fcount);
            for (uint32_t i = 0; i < fcount; ++i) {
                values.push_back(bindless.read_param(gpu.param_offset + u.slot + i));
            }
            return values;
        });

        GpuMaterial updated = bindless.material_at(material.material_id());
        updated.base_params[9] = gi.emission.x;
        updated.base_params[10] = gi.emission.y;
        updated.base_params[11] = gi.emission.z;
        if (gi.gi_albedo_override) {
            updated.base_params[12] = gi.gi_albedo_override->x;
            updated.base_params[13] = gi.gi_albedo_override->y;
            updated.base_params[14] = gi.gi_albedo_override->z;
            updated.has_gi_albedo_override = 1;
        } else {
            updated.has_gi_albedo_override = 0;
        }
        bindless.update_material(material.material_id(), updated);
    }

    return changed;
}

} // namespace fjell
