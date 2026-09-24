#include "ui/inspectors/vfx_module_inspectors.hpp"

#include "core/string_utils.hpp"
#include "renderer/systems/vfx_module.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/kit/curve_editor.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace fjell {

namespace {

ui::Unit unit_of(PinUnit unit) {
    switch (unit) {
        case PinUnit::none: return ui::Unit::None;
        case PinUnit::seconds: return ui::Unit::Seconds;
        case PinUnit::metres: return ui::Unit::Metres;
        case PinUnit::metres_per_second: return ui::Unit::MetresPerSecond;
        case PinUnit::degrees: return ui::Unit::Degrees;
        case PinUnit::times: return ui::Unit::Times;
    }
    return ui::Unit::None;
}

// An angle stored in radians and edited in degrees.
ui::Edit degrees(const char* id, float& radians, const ui::DragSpec& spec) {
    float deg = glm::degrees(radians);
    const ui::Edit edit = ui::drag(id, deg, spec);
    if (edit.changed) radians = glm::radians(deg);
    return edit;
}

// One of a module's values as a row, from its descriptor: the same
// description the graph draws its pin and field from.
ui::Edit pin_row(PinDescriptor& pin) {
    ui::Edit edit;
    const std::string label = pin.label.empty() ? display_name(pin.name) : pin.label;
    const std::string id = "##" + pin.name;
    const ui::DragSpec spec{.speed = pin.drag_speed, .min = pin.range_min, .max = pin.range_max,
                            .unit = unit_of(pin.unit), .format = pin.format != nullptr ? pin.format : "%.2f"};
    const auto field = [&] {
        ImGui::BeginDisabled(pin.disabled);
        switch (pin.data_type) {
            case PinDataType::float_type: {
                auto& v = *static_cast<float*>(pin.value_ptr);
                if (pin.radians) {
                    edit |= degrees(id.c_str(), v, spec);
                } else if (pin.widget == PinWidget::slider) {
                    edit |= ui::slider(id.c_str(), v, pin.range_min, pin.range_max, spec.unit, spec.format);
                } else {
                    edit |= ui::drag(id.c_str(), v, spec);
                }
                break;
            }
            case PinDataType::vec3_type: {
                auto& v = *static_cast<glm::vec3*>(pin.value_ptr);
                if (pin.widget == PinWidget::colour) {
                    edit |= ui::color(id.c_str(), v, ui::ColorSpace::Srgb);
                } else {
                    edit |= ui::vec3(id.c_str(), v, spec);
                }
                break;
            }
            case PinDataType::vec4_type:
                edit |= ui::color(id.c_str(), *static_cast<glm::vec4*>(pin.value_ptr), ui::ColorSpace::Srgb);
                break;
            case PinDataType::uint_type: {
                auto& v = *static_cast<uint32_t*>(pin.value_ptr);
                const int lo = static_cast<int>(pin.range_min);
                int whole = static_cast<int>(v);
                const ui::Edit e = ui::drag_int(id.c_str(), whole, pin.drag_speed, lo, static_cast<int>(pin.range_max));
                if (e.changed) v = static_cast<uint32_t>(std::max(whole, lo));
                edit |= e;
                break;
            }
            case PinDataType::enum_type: {
                auto& v = *static_cast<uint32_t*>(pin.value_ptr);
                std::vector<const char*> names;
                for (const auto& name : pin.enum_names) names.push_back(name.c_str());
                int index = static_cast<int>(v);
                const ui::Edit e = ui::choice(id.c_str(), index, names);
                if (e.changed && index >= 0) v = static_cast<uint32_t>(index);
                edit |= e;
                break;
            }
        }
        ImGui::EndDisabled();
    };
    if (pin.help != nullptr) {
        ui::row(label.c_str(), pin.help, field);
    } else {
        ui::row(label.c_str(), field);
    }
    return edit;
}

// A curve with ready-made shapes under it.
template <typename... Presets>
ui::Edit curve_with_presets(const char* id, Curve& curve, Presets&&... presets) {
    ui::Edit edit;
    edit |= ui::curve_editor(id, curve);
    auto apply = [&](const char* label, Curve shape) {
        if (ui::button(label, ui::ButtonKind::Ghost)) {
            curve = shape;
            edit = {true, true};
        }
        ImGui::SameLine();
    };
    (apply(presets.first, presets.second), ...);
    ImGui::NewLine();
    return edit;
}

} // namespace

ui::Edit draw_vfx_module_fields(VfxModule& module, const InspectorContext& ctx) {
    ui::Edit edit;
    if (auto t = ui::PropertyTable("##module")) {
        // What decides whether another value is used comes before it.
        if (module.type() == ModuleType::wind) {
            auto& m = static_cast<WindModule&>(module);
            ui::row("Global wind", "Blow with the environment's wind, its direction, speed and gusts, times "
                                   "Strength; Direction is then not used.",
                    [&] { edit |= ui::checkbox("##global", m.use_global); });
        }

        // The module's values, as its descriptors say; those its mode leaves
        // unused are not shown.
        auto pins = module.pin_descriptors();
        const char* group = nullptr;
        for (auto& pin : pins) {
            if (!pin.shown) continue;
            if (pin.group != nullptr && (group == nullptr || std::string_view(group) != pin.group)) {
                ui::subheading(pin.group);
            }
            group = pin.group;
            edit |= pin_row(pin);
        }
        if (edit.changed) keep_ranges(pins);

        // What some modules show besides their values.
        switch (module.type()) {
            case ModuleType::flipbook: {
                auto& m = static_cast<FlipbookModule&>(module);
                char frames[64];
                const uint32_t total = m.frame_count > 0 ? m.frame_count : m.columns * m.rows;
                std::snprintf(frames, sizeof(frames), "%u frames on a %u \xc3\x97 %u grid", total, m.columns, m.rows);
                ui::row("Plays", [&] { ui::readout(frames); });
                break;
            }
            case ModuleType::billboard:
                ui::row("Draws", [&] { ui::readout("A quad facing the camera"); });
                break;
            case ModuleType::trail: {
                auto& m = static_cast<TrailModule&>(module);
                ui::row("Width", [&] {
                    edit |= ui::drag("##width", m.width, {.speed = 0.001f, .min = 0.001f, .max = 10.0f,
                                                          .unit = ui::Unit::Metres, .format = "%.3f"});
                });
                ui::row("Width over life", "1 keeps the width to the end; 0 tapers it to nothing.", [&] {
                    edit |= ui::slider("##taper", m.width_over_life, 0.0f, 1.0f);
                });
                ui::row("Face camera", [&] { edit |= ui::checkbox("##face", m.face_camera); });
                break;
            }
            default:
                break;
        }

        // Asset references, through the engine's one asset field.
        for (auto& field : module.asset_fields()) {
            char id[64];
            std::snprintf(id, sizeof(id), "##asset%p", static_cast<const void*>(field.reference));
            ui::row(field.label, [&] {
                if (asset_slot({.label = field.label, .id = id, .extensions = field.extensions, .ctx = &ctx,
                                .shader_types = field.shader_types},
                               *field.reference)) {
                    edit = {true, true};
                }
            });
        }
    }

    // Curves take the panel's width, under the rows.
    if (module.type() == ModuleType::curve_color_over_life) {
        auto& m = static_cast<CurveColorOverLifeModule&>(module);
        const struct {
            const char* label;
            Curve* curve;
        } channels[] = {{"Red", &m.curve_r}, {"Green", &m.curve_g}, {"Blue", &m.curve_b}, {"Alpha", &m.curve_a}};
        for (const auto& channel : channels) {
            if (ui::subheading_foldable(channel.label, false)) {
                ImGui::PushID(channel.label);
                edit |= ui::curve_editor("##curve", *channel.curve);
                ImGui::PopID();
            }
        }
        if (ui::button("Linear", ui::ButtonKind::Ghost)) {
            m.curve_r = Curve::linear(1.0f, 1.0f);
            m.curve_g = Curve::linear(0.8f, 0.1f);
            m.curve_b = Curve::linear(0.2f, 0.0f);
            m.curve_a = Curve::linear(1.0f, 0.0f);
            edit = {true, true};
        }
        ImGui::SameLine();
        if (ui::button("Fade out", ui::ButtonKind::Ghost)) {
            m.curve_r = Curve::linear(1.0f, 1.0f);
            m.curve_g = Curve::linear(1.0f, 1.0f);
            m.curve_b = Curve::linear(1.0f, 1.0f);
            m.curve_a = Curve::ease_in(1.0f, 0.0f);
            edit = {true, true};
        }
        ImGui::SameLine();
        if (ui::button("Pulse", ui::ButtonKind::Ghost)) {
            m.curve_a = Curve::bell(1.0f);
            edit = {true, true};
        }
    } else if (module.type() == ModuleType::curve_size_over_life) {
        auto& m = static_cast<CurveSizeOverLifeModule&>(module);
        edit |= curve_with_presets("##size", m.curve, std::pair{"Linear", Curve::linear(0.1f, 0.02f)},
                                   std::pair{"Shrink", Curve::ease_in(0.1f, 0.0f)},
                                   std::pair{"Grow", Curve::ease_out(0.0f, 0.1f)}, std::pair{"Pulse", Curve::bell(0.15f)});
    }
    return edit;
}

} // namespace fjell
