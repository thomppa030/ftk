#pragma once

#include "ftk/ui/kit/edit.hpp"

#include <span>
#include <string>

namespace fjell {

class Scene;
struct InspectorContext;
struct OwnedFieldDesc;

/// How a Model's rows are drawn.
struct ModelFieldsOptions {
    /// How markup reaches the object (`hud`): each row's help is the path to
    /// its field.
    std::string path{};
    /// The list the object is an entry of (`hud.slots`), with `path` then
    /// the way into the entry; empty for an object that is no list's entry.
    std::string entries_of{};
    /// The values are the caller's to change. Otherwise every row is shown
    /// and none edited: a component read from the scene.
    bool editable{true};
    const InspectorContext* ctx{nullptr};
    const Scene* scene{nullptr}; ///< names the nodes a node reference points at
};

/// Rows for every field of a reflected object at `object`, edited in place
/// (UI editor data panel, kit spec): a value as its kind's field, a getter's
/// value shown and never edited, a struct inside it under a sub-heading, and
/// a list as the kit's cards, grown and shrunk through the functions the game
/// module generated for it. A list of structs shows one card open at a time,
/// the others as a summary line. Labels are sentence case.
ui::Edit draw_model_fields(std::span<const OwnedFieldDesc> fields, void* object, const ModelFieldsOptions& options);

/// The line a closed card shows for a struct: its first field's value, then
/// its first text field's when that is another field and not empty.
[[nodiscard]] std::string model_summary(std::string_view type, std::span<const OwnedFieldDesc> fields,
                                        const void* object);

} // namespace fjell
