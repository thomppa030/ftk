#pragma once

#include "ui/kit/edit.hpp"

#include <string>
#include <string_view>

namespace fjell::ui {

/// A single-line text field that commits on Enter or when it loses focus,
/// never per keystroke, and restores the old text on Escape. `value` is
/// written only on the committing frame, so code that reads it while the
/// user types sees the old text.
///
///     if (ui::text_field("##Default", clip.default_name).committed) {
///         changed = true;
///     }
Edit text_field(const char* id, std::string& value);

/// The same field for a caller that applies the edit itself (through an
/// undo command) and needs to know whether an edit is under way. Kept by
/// the caller as a member, one per field, and never writes
/// `value` itself: the caller applies text() when `committed` is set, which
/// is where a rename command or "unsaved" belongs.
///
///     if (name_field_.draw("##Name", node.name).committed) {
///         rename(node, name_field_.text());
///     }
class TextField {
public:
    /// Draws the field for `value`. `committed` is set only when the
    /// finished text differs from `value`; read it back with text().
    Edit draw(const char* id, std::string_view value);

    /// The field's text: the committed text on a committing frame.
    [[nodiscard]] const std::string& text() const { return text_; }

    /// Whether the field was being typed into as of the last draw(). A
    /// caller that edits a selected object reads this before drawing to keep
    /// the object the edit started on.
    [[nodiscard]] bool editing() const { return editing_; }

private:
    std::string text_;
    bool editing_{false};
};

} // namespace fjell::ui
