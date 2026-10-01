#pragma once

#include "ftk/ui/kit/edit.hpp"

namespace fjell {

struct InspectorContext;
struct Node3D;

// The inspectors of components built from many placed points: splines, a
// mesh laid along a spline, and instanced copies of a mesh.
ui::Edit draw_spline(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_spline_mesh(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_instanced_mesh(Node3D& node, void* component, const InspectorContext& ctx);

} // namespace fjell
