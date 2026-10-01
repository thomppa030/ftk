#pragma once

#include <imgui.h>

// A block of values that belongs to a component but is something of its own,
// set apart in a sunken box: the material a component paints itself with,
// the sky shader's values on the World Environment (sheet 11), a widget's
// Model in the UI editor's Data panel.
//
//     if (auto box = ui::InsetGroup("##material", "Material · crate_wood", icon, colour)) {
//         // rows and sub-headings, fields on the inset colour
//     }

namespace fjell::ui {

/// What a box's heading holds.
struct InsetHeading {
    const char* text{""};
    const char* icon{nullptr};
    ImVec4 icon_colour{};
    /// The heading names the box, bold at the body size and as written (an
    /// input action, sheet 16), rather than labelling a block of values in
    /// small capitals (a material's).
    bool named{false};
    /// A name from code (a Model's `hud`): in the mono face, as written.
    bool code{false};
    /// After the name, in `text_secondary`: what it is (a Model's type).
    const char* detail{nullptr};
    /// A dot in `success` at the heading's right, with this as its tooltip:
    /// the values move on their own, read from a scene that is playing.
    const char* live{nullptr};
    /// An icon button at the heading's right, before the remove icon, that
    /// sets `*action` when clicked: Reset, for values the box owns.
    const char* action_icon{nullptr};
    const char* action_tooltip{nullptr};
    bool* action{nullptr};
    /// Set when the heading's remove icon is clicked; no icon while null.
    bool* remove{nullptr};
    const char* remove_tooltip{"Remove"};
    /// Shown at the heading's right while the box is folded: what it holds.
    const char* folded_summary{nullptr};
};

class InsetGroup {
public:
    /// Draws the box's heading, which folds, open by default, with `icon` in
    /// `icon_colour`. The contents follow while it is open, inside the box.
    InsetGroup(const char* id, const char* heading, const char* icon, const ImVec4& icon_colour);
    InsetGroup(const char* id, const InsetHeading& heading);
    ~InsetGroup();
    InsetGroup(const InsetGroup&) = delete;
    InsetGroup& operator=(const InsetGroup&) = delete;
    InsetGroup(InsetGroup&&) = delete;
    InsetGroup& operator=(InsetGroup&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{true};
    ImGuiID height_id_{0};
    ImVec2 min_{};
    float width_{0.0f};
    float work_right_{0.0f};
    float content_right_{0.0f};
};

} // namespace fjell::ui
