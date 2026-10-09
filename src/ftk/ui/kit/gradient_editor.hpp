#pragma once

#include "ftk/math/color_gradient.hpp"
#include "ftk/ui/kit/edit.hpp"

#include <glm/vec2.hpp>

#include <cstddef>

// A colour gradient as a bar (the approved gradient bar): what the gradient
// makes across it, each stop a pointer under it in its own colour, a hint on
// how to add and remove stops, and the selected stop's colour and position as
// rows under that.
//
//     edit |= ui::gradient_editor("##ramp", node.gradient);
//
// A click on the bar adds a stop there, in the colour already there, and
// selects it; a stop is selected by a click, moved by a drag (Shift for fine
// steps, its place in a readout), and removed by pulling it off the bar or
// by Del. The gradient keeps at least one stop and no more than
// GradientEditorOptions::max_stops. Stops are kept in order of time.

namespace ftk::ui {

struct GradientEditorOptions {
    /// The most stops a click adds up to.
    std::size_t max_stops{64};
};

/// Draws the editor across the space left. `changed` on every frame the
/// gradient moves, `committed` once per finished edit: a drag let go, a stop
/// added or removed, a colour picked.
Edit gradient_editor(const char* id, ColorGradient& gradient, const GradientEditorOptions& options = {});

/// Where the editor drawn under `id` in the current ID scope puts time `t`
/// on screen: across, the bar's; down, the middle of its stops' pointers.
[[nodiscard]] glm::vec2 gradient_editor_stop_point(const char* id, float t);

/// The same across, down the middle of the bar.
[[nodiscard]] glm::vec2 gradient_editor_bar_point(const char* id, float t);

} // namespace ftk::ui
