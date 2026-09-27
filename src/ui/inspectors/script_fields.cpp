#include "ui/inspectors/script_fields.hpp"

#include "core/asset_type_registry.hpp"
#include "core/base_path.hpp"
#include "core/log.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/component_meta.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scripting/fjell/asset.hpp"
#include "scripting/fjell/weak_ref.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/inspectors/asset_rows.hpp"
#include "ui/kit/button.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/text_field.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

namespace fjell {

std::string field_label(const std::string& name) {
    std::string label;
    label.reserve(name.size());
    for (char c : name) label += c == '_' ? ' ' : c;
    if (!label.empty()) {
        label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    }
    return label;
}

namespace {

// A value copied into the kit's vector type and written back when edited:
// the field is plain floats in the script.
template <typename Vec, typename Field>
ui::Edit edit_floats(float* values, Field&& field) {
    Vec v;
    std::memcpy(&v, values, sizeof(Vec));
    const ui::Edit edit = field(v);
    if (edit.changed) std::memcpy(values, &v, sizeof(Vec));
    return edit;
}

ui::Edit draw_asset_ref(const OwnedFieldDesc& f, AssetData& ref, const InspectorContext& ctx) {
    const auto* asset_entry = AssetTypeRegistry::instance().find_by_name(f.ref_type_name);
    if (asset_entry == nullptr || ctx.resource_manager == nullptr) {
        ui::readout(("Handle " + std::to_string(ref.handle_id)).c_str());
        return {};
    }
    std::string path;
    if (ref.handle_id != 0 && asset_entry->path_of) {
        path = asset_entry->path_of(*ctx.resource_manager, ref.handle_id);
    }
    const std::string exts[] = {asset_entry->extension};
    if (!asset_slot({.label = f.name.c_str(), .id = "##ref", .extensions = exts, .ctx = &ctx,
                     .asset_type = asset_entry, .shader_types = f.shader_types},
                    path)) {
        return {};
    }
    if (path.empty()) {
        ref = {};
    } else {
        auto loaded = ctx.resource_manager->load_asset_by_type(
            f.ref_type_name, base_path::resolve_asset(path, project_of(ctx)));
        if (!loaded) {
            FJELL_CORE_ERROR("Asset ref: {}", loaded.error());
            return {};
        }
        ref.handle_id = *loaded;
        ref.asset_type_id = asset_entry->type_id;
    }
    return {true, true};
}

ui::Edit draw_node_ref(WeakRefData& ref, const Scene* scene) {
    std::string target = "None";
    if (ref.id != 0) {
        const Node* node = scene != nullptr ? scene->find(ref.id) : nullptr;
        if (scene == nullptr) {
            target = "Node " + std::to_string(ref.id);
        } else if (node != nullptr && scene->generation_of(ref.id) == ref.generation) {
            target = node->name;
        } else {
            target = "Missing: the node is gone";
        }
    }
    // The name, then the clear button at the end of the row.
    const float start = ImGui::GetCursorPosX();
    const float clear_at = start + std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight(), 0.0f);
    ui::readout(target.c_str());
    ImGui::SameLine(clear_at);
    ImGui::BeginDisabled(ref.id == 0);
    const bool cleared = ui::icon_button("##clear", ui::icon::clear, "Clear");
    ImGui::EndDisabled();
    if (!cleared) return {};
    ref = {};
    return {true, true};
}

} // namespace

ui::Edit draw_script_field(const OwnedFieldDesc& f, void* ptr, const InspectorContext& ctx, const Scene* scene) {
    const float speed = f.speed > 0.0f ? f.speed : 0.1f;
    const ui::DragSpec spec{.speed = speed, .min = f.min, .max = f.max};
    switch (f.type) {
        case FJELL_FIELD_FLOAT:
            return ui::drag("##value", *static_cast<float*>(ptr), spec);
        case FJELL_FIELD_INT:
            return ui::drag_int("##value", *static_cast<int*>(ptr), speed, static_cast<int>(f.min),
                                static_cast<int>(f.max));
        case FJELL_FIELD_BOOL:
            return ui::checkbox("##value", *static_cast<bool*>(ptr));
        case FJELL_FIELD_VEC2:
            return edit_floats<glm::vec2>(static_cast<float*>(ptr),
                                          [&](glm::vec2& v) { return ui::vec2("##value", v, spec); });
        case FJELL_FIELD_VEC3:
            return edit_floats<glm::vec3>(static_cast<float*>(ptr),
                                          [&](glm::vec3& v) { return ui::vec3("##value", v, spec); });
        case FJELL_FIELD_VEC4:
            return edit_floats<glm::vec4>(static_cast<float*>(ptr),
                                          [&](glm::vec4& v) { return ui::vec4("##value", v, spec); });
        // Script colours are sRGB, as authored and as saved.
        case FJELL_FIELD_COLOR3:
            return edit_floats<glm::vec3>(static_cast<float*>(ptr), [](glm::vec3& v) {
                return ui::color("##value", v, ui::ColorSpace::Srgb);
            });
        case FJELL_FIELD_COLOR4:
            return edit_floats<glm::vec4>(static_cast<float*>(ptr), [](glm::vec4& v) {
                return ui::color("##value", v, ui::ColorSpace::Srgb);
            });
        case FJELL_FIELD_STRING: {
            // A fixed buffer in the script, written back truncated to fit.
            auto* buf = static_cast<char*>(ptr);
            if (f.size == 0) return {};
            std::string text(buf, std::find(buf, buf + f.size, '\0'));
            if (!ui::text_field("##value", text).committed) return {};
            const std::size_t n = std::min(text.size(), static_cast<std::size_t>(f.size) - 1);
            std::memcpy(buf, text.data(), n);
            buf[n] = '\0';
            return {true, true};
        }
        case FJELL_FIELD_STD_STRING: {
            const bool committed = ui::text_field("##value", *static_cast<std::string*>(ptr)).committed;
            return {committed, committed};
        }
        case FJELL_FIELD_ENUM: {
            if (f.enum_labels.empty()) return {};
            std::vector<const char*> names;
            names.reserve(f.enum_labels.size());
            for (const auto& name : f.enum_labels) names.push_back(name.c_str());
            auto& value = *static_cast<int32_t*>(ptr);
            int index = std::clamp(value, 0, static_cast<int>(names.size()) - 1);
            const ui::Edit edit = ui::choice("##value", index, names);
            if (edit.changed) value = index;
            return edit;
        }
        case FJELL_FIELD_WEAK_REF:
            return draw_node_ref(*static_cast<WeakRefData*>(ptr), scene);
        case FJELL_FIELD_ASSET:
            return draw_asset_ref(f, *static_cast<AssetData*>(ptr), ctx);
        case FJELL_FIELD_STRUCT:
        case FJELL_FIELD_VECTOR:
            return {}; // the UI's to show; draw_script_fields gives them no row
    }
    return {};
}

ui::Edit draw_script_fields(const std::vector<OwnedFieldDesc>& fields, void* instance,
                            const InspectorContext& ctx, const Scene* scene) {
    auto* base = static_cast<uint8_t*>(instance);
    ui::Edit edit;
    std::optional<ui::PropertyTable> table;
    std::string category;
    bool open = true;
    bool first = true;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        // A getter has no storage to edit, and a struct or list is the UI's
        // to show; neither has an inspector row.
        if (f.is_getter() || f.type == FJELL_FIELD_STRUCT || f.type == FJELL_FIELD_VECTOR) continue;
        if (first || f.category != category) {
            first = false;
            table.reset();
            category = f.category;
            open = category.empty() || ui::subheading_foldable(category.c_str());
            if (open) table.emplace(("##fields" + std::to_string(i)).c_str());
        }
        if (!open || !table || !*table) continue;

        const std::string label = field_label(f.name);
        ImGui::PushID(static_cast<int>(i));
        ui::row(label.c_str(), f.tooltip.empty() ? nullptr : f.tooltip.c_str(), [&] {
            // Runtime state is shown, never edited: the game writes it.
            ImGui::BeginDisabled(f.readonly || f.is_state());
            edit |= draw_script_field(f, base + f.offset, ctx, scene);
            ImGui::EndDisabled();
        });
        ImGui::PopID();
    }
    return edit;
}

} // namespace fjell
