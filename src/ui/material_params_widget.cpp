#include "ui/material_params_widget.hpp"

#include "core/string_utils.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/fjsl_parser.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_gi_params.hpp"
#include "renderer/uniform_types.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"

#include <imgui.h>

#include <string>
#include <string_view>
#include <vector>

namespace fjell {

bool draw_uniform_value(const FjslUniform& u, std::span<float> values) {
    float* v = values.data();
    switch (u.type) {
        case UniformType::Float:
            if (u.hint == UniformHint::None && u.range.max > u.range.min) {
                return ui::slider("##val", *v, u.range.min, u.range.max).changed;
            }
            return ui::drag("##val", *v, {.speed = 0.01f}).changed;
        case UniformType::Vec2: {
            glm::vec2 value{v[0], v[1]};
            const bool changed = ui::vec2("##val", value, {.speed = 0.01f}).changed;
            v[0] = value.x;
            v[1] = value.y;
            return changed;
        }
        case UniformType::Vec3: {
            glm::vec3 value{v[0], v[1], v[2]};
            // The values arrive as a person reads them, a colour in sRGB.
            const bool changed = u.hint == UniformHint::Color
                ? ui::color("##val", value, ui::ColorSpace::Srgb).changed
                : ui::vec3("##val", value, {.speed = 0.01f}).changed;
            v[0] = value.x;
            v[1] = value.y;
            v[2] = value.z;
            return changed;
        }
        case UniformType::Vec4: {
            glm::vec4 value{v[0], v[1], v[2], v[3]};
            const bool changed = u.hint == UniformHint::Color
                ? ui::color("##val", value, ui::ColorSpace::Srgb).changed
                : ui::vec4("##val", value, {.speed = 0.01f}).changed;
            v[0] = value.x;
            v[1] = value.y;
            v[2] = value.z;
            v[3] = value.w;
            return changed;
        }
        default:
            return false;
    }
}

bool draw_material_params(const ShaderMetadata& meta,
                           BindlessTextures& bindless,
                           Material& material) {
    bool changed = false;

    // Base PBR params (base_params[4]/[5] — see GpuMaterial in bindless.hpp).
    // These exist on every surface material regardless of shader — including
    // custom FJSL shaders like solid_lit.fjsl that only ever declare their
    // own uniforms (tint, etc.) and never reference ROUGHNESS/METALLIC in
    // fragment(), where they still fall through to these factors. Shown
    // above the custom uniforms so a shader with none still gets an editable
    // material. A ui shader paints a quad and reads neither, and a terrain
    // shader takes them from its layers, so both show only their own uniforms.
    if (meta.shader_type == ShaderType::Spatial && ui::subheading_foldable("Base PBR")) {
        GpuMaterial base = bindless.material_at(material.material_id());
        float metallic = base.base_params[4];
        float roughness = base.base_params[5];
        bool base_changed = false;
        if (auto t = ui::PropertyTable("##base_pbr")) {
            ui::row("Metallic", [&] {
                base_changed |= ui::slider("##metallic", metallic, 0.0f, 1.0f).changed;
            });
            ui::row("Roughness", [&] {
                base_changed |= ui::slider("##roughness", roughness, 0.0f, 1.0f).changed;
            });
        }
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

    // A shader that names its groups is read as a handful of subjects, each
    // under a heading that folds, rather than one wall of sliders. The
    // uniforms before the first named group sit under no heading.
    const auto& uniforms = meta.uniforms;
    std::size_t start = 0;
    while (start < uniforms.size()) {
        const std::string group = uniforms[start].group;
        std::size_t end = start;
        while (end < uniforms.size() && uniforms[end].group == group) ++end;

        const bool open = group.empty() || ui::subheading_foldable(group.c_str());
        if (open) {
            ImGui::PushID(group.c_str());
            if (auto t = ui::PropertyTable("##uniforms")) {
                for (std::size_t i = start; i < end; ++i) {
                    const auto& u = uniforms[i];
                    if (u.type == UniformType::Sampler2D) continue;
                    const uint32_t fcount = uniform_float_count(u.type);
                    if (u.slot + fcount > param_count) continue;
                    const uint32_t abs_offset = gpu.param_offset + u.slot;
                    const std::string label = display_name(u.name);
                    ImGui::PushID(u.name.c_str());
                    ui::row(label.c_str(), [&] {
                        // Read the current values from the CPU-side params buffer.
                        float temp[4]{};
                        for (uint32_t k = 0; k < fcount; ++k) temp[k] = bindless.read_param(abs_offset + k);
                        decode_uniform_from_gpu(u, std::span{temp}.first(fcount));
                        if (draw_uniform_value(u, std::span{temp}.first(fcount))) {
                            encode_uniform_for_gpu(u, std::span{temp}.first(fcount));
                            bindless.write_params(abs_offset, temp, fcount);
                            changed = true;
                        }
                    });
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }
        start = end;
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
        }, gpu.has_emissive_texture != 0U);

        GpuMaterial updated = bindless.material_at(material.material_id());
        updated.base_params[9] = gi.emission.x;
        updated.base_params[10] = gi.emission.y;
        updated.base_params[11] = gi.emission.z;
        const glm::vec3 base_rgb = glm::vec3{material.params().base_color_factor} * gi.albedo;
        updated.base_params[0] = base_rgb.x;
        updated.base_params[1] = base_rgb.y;
        updated.base_params[2] = base_rgb.z;
        bindless.update_material(material.material_id(), updated);
    }

    return changed;
}

} // namespace fjell
