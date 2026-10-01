#pragma once

namespace ftk::ui {

/// What a field did this frame. `changed` is true on every frame the value
/// moved (for a live preview); `committed` is true once, when the edit is
/// finished — on Enter, when the field loses focus or when a drag is let
/// go — and is what undo and "unsaved" hang off, so one drag is one step.
struct Edit {
    bool changed{false};
    bool committed{false};

    /// Both halves of two edits, for a group of fields that report as one.
    Edit& operator|=(const Edit& other) {
        changed = changed || other.changed;
        committed = committed || other.committed;
        return *this;
    }
};

} // namespace ftk::ui
