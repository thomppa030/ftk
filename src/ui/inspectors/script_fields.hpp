#pragma once

#include "scripting/script_manager.hpp" // OwnedFieldDesc
#include "ui/kit/edit.hpp"

#include <string>
#include <vector>

namespace fjell {

class Scene;
struct InspectorContext;

/// Property rows for an instance's reflected fields: numbers, vectors,
/// colours, enums, text, and asset and node references. Fields with a
/// category go under a sub-heading of that name that folds. A field's
/// tooltip is its row's help. `scene` names the nodes a node reference
/// points at; without one the reference shows its id. Used for script
/// components and animation notifies alike.
ui::Edit draw_script_fields(const std::vector<OwnedFieldDesc>& fields, void* instance,
                            const InspectorContext& ctx, const Scene* scene);

/// One field's value, for the value column of a row the caller opened:
/// `ptr` is the field's storage. A struct, a list or a getter draws nothing.
ui::Edit draw_script_field(const OwnedFieldDesc& field, void* ptr, const InspectorContext& ctx, const Scene* scene);

/// A field's row label: `move_speed` reads "Move speed", the sentence case
/// every label uses.
[[nodiscard]] std::string field_label(const std::string& name);

} // namespace fjell
