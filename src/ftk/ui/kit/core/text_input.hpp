#pragma once

// The kit's text editing on the UI core, under every field that takes
// text: a text field and entry, a number being typed, a hex code, a rename,
// and a text area of several lines. It edits as ImGui's InputText does (decision 15 of the UI core
// plan): the keyboard goes to it when it is pressed or given it, and while
// it has it, typing, the caret and selection by keys and the mouse, the
// clipboard, undo, the input method's composition, Enter and Escape.
// Inside ftk-app-ui only.

#include "ftk/ui/kit/core/kit.hpp"

#include <cstdint>
#include <string>

namespace ftk::ui::corekit {

/// What Escape does to a text box's text; the box lets the keyboard go
/// either way.
enum class EscapeDoes : uint8_t {
    /// Puts it back as it was when the box took the keyboard.
    Revert,
    /// Empties it: a search.
    Clear,
    /// Leaves it as typed: a find, which Escape closes.
    Keep,
};

struct TextInputOptions {
    /// Shown dimmed while the text is empty.
    const char* hint{nullptr};
    /// Taking the keyboard selects the whole text, as a click does too.
    bool select_all{false};
    /// Room left of the text, past the frame's padding: an icon's.
    float lead{0.0f};
    /// Room right of the text, before the frame's padding: buttons inside
    /// the box's end.
    float trail{0.0f};
    EscapeDoes escape{EscapeDoes::Revert};
    /// Enter leaves the keyboard with the box: a find, where Enter goes on
    /// to the next match.
    bool enter_keeps{false};
};

/// What a text box did this frame.
struct TextInput {
    /// The text changed this frame, and `value` with it.
    bool changed{false};
    /// Enter was pressed in it; it lets the keyboard go unless asked to
    /// keep it.
    bool entered{false};
    /// Escape was pressed in it; it lets the keyboard go.
    bool escaped{false};
    /// It took the keyboard this frame, or lost it: left, entered or
    /// cancelled.
    bool activated{false};
    bool deactivated{false};
    /// Losing it, it had changed since it took it; or Escape put it back.
    bool edited{false};
    bool cancelled{false};
};

/// A one-line text box at `rect` for `value`, which it edits live while it
/// has the keyboard and puts back as it was on Escape. It is the item just
/// drawn, an item the keyboard edits.
TextInput text_input(Id id, core::Rect rect, std::string& value, const TextInputOptions& options = {});

/// The same box for text of several lines, wrapped at its width and
/// scrolled to keep the caret in sight: Up and Down go from line to line,
/// Home and End to a line's ends (with Ctrl, the text's), Enter starts a new
/// line and Ctrl Enter is the Enter that finishes. `lead`, `trail` and
/// `enter_keeps` are a one-line box's.
TextInput text_area_input(Id id, core::Rect rect, std::string& value, const TextInputOptions& options = {});

} // namespace ftk::ui::corekit
