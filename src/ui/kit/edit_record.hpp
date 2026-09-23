#pragma once

#include "ui/kit/edit.hpp"

#include <imgui.h>

#include <optional>
#include <string>

// What a finished edit changed, in the words the field shows: its row's
// label, and its value before and after ("Mass", "1.00 kg", "2.50 kg").
// Every kit field writes one when its edit is finished; the Properties
// panel reads it to name the undo step ("Crate · Rigidbody: Mass 1.00 kg ->
// 2.50 kg").
//
// Records belong to a scope, one block of fields such as one component of
// one node, so an edit is only ever described by a field of its own block.
//
//     ui::edit_scope("12/rigidbody");
//     ...draw the component's fields...
//     ui::edit_scope({});
//     if (auto record = ui::take_edit_record("12/rigidbody")) { ... }

namespace fjell::ui {

struct EditRecord {
    std::string label{};
    std::string before{};
    std::string after{};
};

/// The fields drawn from here on record their edits under `scope`, until
/// the next call. An empty scope records nothing.
void edit_scope(std::string scope);

/// The field edit finished in `scope` since the last take, when there is
/// exactly one and it happened under a row label. None when the edit
/// spanned several fields or none (Fit to mesh, adding a list item).
[[nodiscard]] std::optional<EditRecord> take_edit_record(const std::string& scope);

namespace detail {

/// The label of the row being drawn; set by ui::row, cleared where fields
/// are drawn outside rows (a list card).
void set_edit_label(const char* label);

/// Called by a field after it is drawn, with its text as it read before
/// this frame (`before`) and as it reads now, what it reported, and whether
/// its edit started this frame. The text at the start of the edit is kept
/// under `id` until the edit is finished. A field drawn inside another (the
/// hex code in a colour field) records first and is replaced by the outer
/// field on the same frame.
void track_field(ImGuiID id, const std::string& before, const std::string& after, bool started,
                 const Edit& edit);

/// A number with its unit, the way the field shows it ("2.50 kg", "90°").
[[nodiscard]] std::string with_unit(const char* number, const char* unit_symbol);

} // namespace detail

} // namespace fjell::ui
