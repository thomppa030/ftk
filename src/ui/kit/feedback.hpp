#pragma once

// How the editor tells you something that isn't a value.

namespace fjell::ui {

/// A panel with nothing to show: the icon, what the state is ("Nothing
/// selected") and one line of what to do about it, centred in the space
/// left in the window. With an `action`, a button for the one obvious thing
/// to do; returns true when it is clicked. Plain statements, no full stops.
bool empty_state(const char* icon, const char* title, const char* what_to_do,
                 const char* action = nullptr);

} // namespace fjell::ui
