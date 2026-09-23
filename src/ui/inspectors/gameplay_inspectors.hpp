#pragma once

#include "ui/kit/edit.hpp"

namespace fjell {

struct InspectorContext;
struct Node3D;

// The audio, effect, game UI, input and marker components' fields, for the
// inspector registry.
ui::Edit draw_audio_source(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_particle_emitter(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_ui_widget(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_input(Node3D& node, void* component, const InspectorContext& ctx);
ui::Edit draw_arrow(Node3D& node, void* component, const InspectorContext& ctx);

} // namespace fjell
