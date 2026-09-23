#pragma once

#include "ui/kit/edit.hpp"

namespace fjell {

struct InspectorContext;
struct Node3D;

// The rendering components' fields, for the inspector registry.
ui::Edit draw_mesh(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_skeletal_mesh(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_sprite(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_tilemap(Node3D& node, void* component, const InspectorContext& ctx);

} // namespace fjell
