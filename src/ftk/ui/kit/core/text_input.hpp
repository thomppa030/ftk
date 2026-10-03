#pragma once

// The kit's one-line text editing on the UI core, under every field that
// takes text: a text field and entry, a number being typed, a hex code, a
// rename. It edits as ImGui's InputText does (decision 15 of the UI core
// plan): the keyboard goes to it when it is pressed or given it, and while
// it has it, typing, the caret and selection by keys and the mouse, the
// clipboard, undo, the input method's composition, Enter and Escape.
// Inside ftk-app-ui only.

#include "ftk/ui/kit/core/kit.hpp"

#include <string>

namespace ftk::ui::corekit {

struct TextInputOptions {
    /// Shown dimmed while the text is empty.
    const char* hint{nullptr};
    /// Taking the keyboard selects the whole text, as a click does too.
    bool select_all{false};
};

/// What a text box did this frame.
struct TextInput {
    /// The text changed this frame, and `value` with it.
    bool changed{false};
    /// Enter was pressed in it; it lets the keyboard go.
    bool entered{false};
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

} // namespace ftk::ui::corekit
