#pragma once

// How the editor tells you something that isn't a value.

namespace fjell::ui {

enum class Severity { Success, Warning, Error };

/// One line under the thing it is about, the whole line in the severity's
/// colour behind its icon: a steepness that may fold waves, a material of
/// the wrong type. Wraps to the space left. A problem that stops something
/// working until it is fixed is a callout, not a status line.
void status(Severity severity, const char* text);

/// A panel with nothing to show: the icon, what the state is ("Nothing
/// selected") and one line of what to do about it, centred in the space
/// left in the window. With an `action`, a button for the one obvious thing
/// to do; returns true when it is clicked. Plain statements, no full stops.
bool empty_state(const char* icon, const char* title, const char* what_to_do,
                 const char* action = nullptr);

} // namespace fjell::ui
