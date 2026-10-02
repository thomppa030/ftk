#pragma once

// Where the next item goes. Items go down the window, one under the other,
// unless the next is put on the same line as the last.
//
//     ui::button("Apply");
//     ui::same_line();
//     ui::button("Revert");
//     ui::separator();

namespace ftk::ui {

/// The next item goes on the same line as the last, the usual space after
/// it.
void same_line();

/// The same, `gap` pixels after it.
void same_line(float gap);

/// A rule across the space left, between groups of items.
void separator();

} // namespace ftk::ui
