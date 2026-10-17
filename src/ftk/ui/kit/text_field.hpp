#pragma once

#include "ftk/ui/kit/edit.hpp"

#include <string>
#include <string_view>

namespace ftk::ui {

/// A single-line text field that commits on Enter or when it loses focus,
/// never per keystroke, and restores the old text on Escape. `value` is
/// written only on the committing frame, so code that reads it while the
/// user types sees the old text.
///
///     if (ui::text_field("##Default", clip.default_name).committed) {
///         changed = true;
///     }
Edit text_field(const char* id, std::string& value);

struct TextFieldOptions {
    /// Shown dimmed while the field is empty: what an empty field stands
    /// for ("Beside each source").
    const char* hint{nullptr};
};

/// The same field, with options.
Edit text_field(const char* id, std::string& value, TextFieldOptions options);

struct TextEntryOptions {
    /// Shown dimmed while the field is empty.
    const char* hint{nullptr};
    /// What the field begins with before the typing, in the secondary text
    /// colour: a prompt, such as the `:` before a line to go to.
    const char* prompt{nullptr};
    /// The keyboard goes to the field on this draw.
    bool focus{false};
    /// Given the keyboard by `focus`, the caret goes after the text with
    /// nothing selected, rather than selecting it all: a field filled in to
    /// be added to, such as a new tag's name after its parent's.
    bool caret_at_end{false};
};

/// A field whose text is live, unlike text_field's: `value` is written as
/// it is typed, for a name a button waits on or a path to go to, where
/// nothing is committed until the caller acts. Returns true on the frame
/// Enter is pressed in it; item_deactivated() after it says it was left.
///
///     if (ui::text_entry("##name", name, {.hint = "Project name"})) create(name);
bool text_entry(const char* id, std::string& value, TextEntryOptions options = {});

struct TextAreaOptions {
    /// Shown dimmed while the area is empty.
    const char* hint{nullptr};
    /// How many lines high the area is; longer text scrolls inside it.
    int lines{3};
};

/// A field of several lines, its text wrapped at the field's width, for a
/// sentence or a paragraph: a description. Enter starts a new line; Ctrl
/// Enter, or leaving it, finishes. It commits as text_field does: on
/// finishing, never per keystroke, and Escape puts the old text back.
///
///     if (ui::text_area("##Description", details.description).committed) changed = true;
Edit text_area(const char* id, std::string& value, TextAreaOptions options = {});

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

} // namespace ftk::ui
