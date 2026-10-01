#pragma once

#include "ftk/ui/kit/edit.hpp"

namespace fjell {

struct InspectorContext;
struct Node3D;

// The animation components' fields, for the inspector registry.
ui::Edit draw_animator(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_sprite_animation(Node3D& node, void* component, const InspectorContext& ctx);

} // namespace fjell
