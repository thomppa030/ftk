#pragma once

#include "ftk/ui/theme.hpp"

#include <string_view>

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
// fold; the fold state is remembered under the heading's ID.

namespace ftk::ui {

/// A section heading. The label is written as it reads ("Transform"); it
/// is drawn uppercase. `icon` is the section's own, by meaning.
void section(const char* label, const char* icon);

/// A section that folds. Returns true while it is open.
bool section_foldable(const char* label, const char* icon, bool default_open = true);

/// What a section's heading holds beside its label and icon.
struct SectionHeading {
    const char* label{""};
    const char* icon{nullptr};
    /// After the label, as written and in the disabled text colour: how
    /// much the section holds ("5 scenes · 2 left").
    std::string_view count{};
    /// An icon button at the heading's end while the section is open (its
    /// view settings), which sets `*end_pressed` when clicked; none while
    /// `end_pressed` is null.
    const char* end_icon{nullptr};
    const char* end_tooltip{nullptr};
    bool* end_pressed{nullptr};
};

/// A section that folds, its heading as `heading` says. Returns true while
/// it is open.
bool section_foldable(const SectionHeading& heading, bool default_open = true);

/// Opens the section `label`, drawn in the scope this is called in, as a
/// click on its heading would: for an owner bringing something folded in it
/// into view.
void open_section(const char* label);

/// A sub-heading inside a section or a component.
void subheading(const char* label);

/// A sub-heading that folds. Returns true while it is open.
bool subheading_foldable(const char* label, bool default_open = true);

/// A sub-heading for a group of things of one kind, led by its hue's
/// dot ("Physics" in the Add Component picker).
bool subheading_foldable(const char* label, theme::Hue hue, bool default_open = true);

} // namespace ftk::ui
