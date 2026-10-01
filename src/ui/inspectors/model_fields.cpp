#include "ui/inspectors/model_fields.hpp"
#include "scripting/script_call.hpp"

#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/section.hpp"
#include "scripting/reflected_types.hpp"
#include "ui/game_ui/ui_value.hpp"
#include "ui/inspectors/script_fields.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

#include <optional>
#include <string>

namespace fjell {

namespace {

/// A getter's value, got into a value of its type and shown in the field
/// its kind has, never edited.
template <typename T>
void show_getter(const OwnedFieldDesc& field, const void* object, const ModelFieldsOptions& options) {
    T value{};
    // A getter is game code: through call_script(), so what it throws is
    // reported and reads as the default.
    const ScriptCall call{.type_name = field.name, .instance = object};
    call_script(call, [&] { field.get(object, &value); });
    ImGui::BeginDisabled();
    (void)draw_script_field(field, &value, *options.ctx, options.scene);
    ImGui::EndDisabled();
}

void draw_getter(const OwnedFieldDesc& field, const void* object, const ModelFieldsOptions& options) {
    if (field.get == nullptr) return;
    switch (field.type) {
        case FJELL_FIELD_BOOL: show_getter<bool>(field, object, options); break;
        case FJELL_FIELD_INT: show_getter<int32_t>(field, object, options); break;
        case FJELL_FIELD_FLOAT: show_getter<float>(field, object, options); break;
        case FJELL_FIELD_VEC2: show_getter<glm::vec2>(field, object, options); break;
        case FJELL_FIELD_VEC3: show_getter<glm::vec3>(field, object, options); break;
        case FJELL_FIELD_VEC4: show_getter<glm::vec4>(field, object, options); break;
        case FJELL_FIELD_STD_STRING: show_getter<std::string>(field, object, options); break;
        default: break; // fjreflect accepts no other return type
    }
}

/// Where markup reads a field: `hud.selected_name`, or for an entry of a
/// list, the entry's field and the list.
std::string path_to(const ModelFieldsOptions& options, const std::string& name) {
    const std::string at = options.path.empty() ? name : options.path + "." + name;
    return options.entries_of.empty() ? at : "Each entry's " + at + " in " + options.entries_of;
}

/// An element of a list of plain values, as a field of the element's kind.
OwnedFieldDesc element_field(const OwnedVectorDesc& vector) {
    OwnedFieldDesc field;
    field.name = "value";
    field.type = vector.element;
    field.offset = 0;
    field.size = vector.element_size;
    field.min = 0.0f;
    field.max = 0.0f;
    field.speed = 0.0f;
    return field;
}

ui::Edit draw_list(const OwnedFieldDesc& field, void* list, const ModelFieldsOptions& options) {
    const OwnedVectorDesc& vector = field.vector;
    if (vector.size == nullptr || vector.at == nullptr) return {};
    const bool structs = vector.element == FJELL_FIELD_STRUCT && vector.element_type != nullptr;
    const bool editable = options.editable && vector.insert != nullptr && vector.erase != nullptr;
    // Which card of a list of structs is open is the view's, kept with the list.
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID("##open_card");
    int open = storage->GetInt(open_id, -1);

    ui::Edit edit;
    std::optional<uint32_t> remove;
    std::optional<std::pair<std::size_t, std::size_t>> moved;
    bool add = false;
    const auto count = vector.size(list);
    ModelFieldsOptions inner = options;
    inner.path.clear();
    inner.entries_of = path_to(options, field.name);
    {
        ui::ListEditor cards("##cards", count,
                             {.reorder = editable && vector.move != nullptr, .remove = editable, .opens = structs});
        for (uint32_t i = 0; i < count; ++i) {
            const bool is_open = structs && static_cast<int>(i) == open;
            if (!cards.begin_item(i, is_open)) continue;
            // `at` points into the list, which is ours to change while editable.
            void* entry = const_cast<void*>(vector.at(list, i)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
            ImGui::PushID(static_cast<int>(i));
            if (structs && is_open) {
                edit |= draw_model_fields(vector.element_type->fields, entry, inner);
            } else if (structs) {
                const std::string summary =
                    model_summary(vector.element_type->name, vector.element_type->fields, entry);
                ImGui::TextUnformatted(summary.empty() ? "Empty" : summary.c_str());
            } else if (auto table = ui::PropertyTable("##value")) {
                ui::row("Value", [&] {
                    ImGui::BeginDisabled(!options.editable);
                    edit |= draw_script_field(element_field(vector), entry, *options.ctx, options.scene);
                    ImGui::EndDisabled();
                });
            }
            ImGui::PopID();
            if (cards.end_item()) remove = i;
            if (cards.clicked()) open = is_open ? -1 : static_cast<int>(i);
        }
        if (count == 0) cards.empty("No entries");
        if (editable) add = cards.add_button("Add entry");
        moved = cards.moved();
    }
    if (add) {
        vector.insert(list, count);
        open = static_cast<int>(count);
        edit = {true, true};
    } else if (remove) {
        vector.erase(list, *remove);
        if (open == static_cast<int>(*remove)) open = -1;
        else if (open > static_cast<int>(*remove)) --open;
        edit = {true, true};
    } else if (moved && vector.move != nullptr) {
        const auto [from, before] = *moved;
        const auto to = ui::index_after_move(from, from, before);
        vector.move(list, static_cast<uint32_t>(from), static_cast<uint32_t>(to));
        if (open >= 0) open = static_cast<int>(ui::index_after_move(static_cast<std::size_t>(open), from, before));
        edit = {true, true};
    }
    storage->SetInt(open_id, open);
    return edit;
}

} // namespace

std::string model_summary(std::string_view type, std::span<const OwnedFieldDesc> fields, const void* object) {
    if (fields.empty()) return {};
    const UiValue value(UiObject{type, fields, object});
    std::string summary = value.member(fields.front().name).text();
    for (const auto& field : fields) {
        if (field.type != FJELL_FIELD_STD_STRING && field.type != FJELL_FIELD_STRING) continue;
        if (&field == &fields.front()) break;
        const std::string text = value.member(field.name).text();
        if (!text.empty()) summary += summary.empty() ? text : " \xc2\xb7 " + text; // " · "
        break;
    }
    return summary;
}

ui::Edit draw_model_fields(std::span<const OwnedFieldDesc> fields, void* object, const ModelFieldsOptions& options) {
    auto* base = static_cast<char*>(object);
    ui::Edit edit;
    std::optional<ui::PropertyTable> table;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        const std::string label = field_label(f.name);
        if (!f.is_getter() && (f.type == FJELL_FIELD_STRUCT || f.type == FJELL_FIELD_VECTOR)) {
            // A struct or a list has its own heading, between tables of rows.
            table.reset();
            ImGui::PushID(static_cast<int>(i));
            ui::subheading(label.c_str());
            if (f.type == FJELL_FIELD_VECTOR) {
                edit |= draw_list(f, base + f.offset, options);
            } else if (f.struct_type != nullptr) {
                ModelFieldsOptions inner = options;
                inner.path = options.path.empty() ? f.name : options.path + "." + f.name;
                edit |= draw_model_fields(f.struct_type->fields, base + f.offset, inner);
            }
            ImGui::PopID();
            continue;
        }
        if (!table) table.emplace(("##rows" + std::to_string(i)).c_str());
        if (!*table) continue;
        const std::string path = path_to(options, f.name);
        ImGui::PushID(static_cast<int>(i));
        ui::row(label.c_str(), path.c_str(), [&] {
            if (f.is_getter()) {
                draw_getter(f, object, options);
                return;
            }
            ImGui::BeginDisabled(!options.editable);
            edit |= draw_script_field(f, base + f.offset, *options.ctx, options.scene);
            ImGui::EndDisabled();
        });
        ImGui::PopID();
    }
    return edit;
}

} // namespace fjell
