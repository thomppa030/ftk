#pragma once

#include "ftk/ui/theme.hpp"

// Headings, in three levels and no fourth:
//
//   section      a part of the panel (Transform, Fog Volume, Components):
//                uppercase, a rule above, its own icon
//   component    one thing on a node: a bar in its hue over a framed header
//                (drawn by the inspector, not here)
//   subheading   a named group inside a section or component: uppercase,
//                smaller, no rule, no icon
//
// Sections belong to the panel, never inside a component. Any level may
// fold; the fold state is remembered by ImGui under the heading's ID.

namespace ftk::ui {

/// A section heading. The label is written as it reads ("Transform"); it
/// is drawn uppercase. `icon` is the section's own, by meaning.
void section(const char* label, const char* icon);

/// A section that folds. Returns true while it is open.
bool section_foldable(const char* label, const char* icon, bool default_open = true);

/// A sub-heading inside a section or a component.
void subheading(const char* label);

/// A sub-heading that folds. Returns true while it is open.
bool subheading_foldable(const char* label, bool default_open = true);

/// A sub-heading for a group of things of one kind, led by its hue's
/// dot ("Physics" in the Add Component picker).
bool subheading_foldable(const char* label, theme::Hue hue, bool default_open = true);

} // namespace ftk::ui
