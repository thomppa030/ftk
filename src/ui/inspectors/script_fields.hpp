#pragma once

#include "scripting/script_manager.hpp" // OwnedFieldDesc
#include "ui/kit/edit.hpp"

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

} // namespace fjell
