#pragma once

#include "ftk/ui/kit/edit.hpp"

namespace fjell {

struct InspectorContext;
struct Node3D;

// The environment components' fields, for the inspector registry.
ui::Edit draw_terrain(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_water(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_motion_source(Node3D& node, void* component, const InspectorContext& ctx);

} // namespace fjell
