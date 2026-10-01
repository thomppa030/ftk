#pragma once

#include "ftk/ui/kit/edit.hpp"

namespace fjell {

struct InspectorContext;
struct Node3D;

// The physics components' fields, for the inspector registry.
ui::Edit draw_rigidbody(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_collider(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_character(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_buoyancy(Node3D& node, void* component, const InspectorContext& ctx);

} // namespace fjell
