#include "ui/material_params_widget.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/uniform_types.hpp"

#include <imgui.h>

namespace fjell {

bool draw_material_params(const ShaderMetadata& meta,
                           BindlessTextures& bindless,
                           Material& material) {
    uint32_t param_count = material.custom_param_count();
    if (param_count == 0) return false;

    const auto& gpu = bindless.material_at(material.material_id());
    bool changed = false;

    for (const auto& u : meta.uniforms) {
        if (u.type == UniformType::Sampler2D) continue;

        uint32_t fcount = uniform_float_count(u.type);
        if (u.slot + fcount > param_count) continue;

        uint32_t abs_offset = gpu.param_offset + u.slot;

        ImGui::PushID(u.name.c_str());
        ImGui::Text("%s", u.name.c_str());
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

    return changed;
}

} // namespace fjell
