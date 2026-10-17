#pragma once

#include "ftk/ui/draw_list.hpp"

#include <glm/vec2.hpp>

#include <cstddef>
#include <optional>
#include <span>

// How the editor tells you something that isn't a value.

namespace ftk::ui {

enum class Severity { Success, Warning, Error };

/// One line under the thing it is about, the whole line in the severity's
/// colour behind its icon: a steepness that may fold waves, a material of
/// the wrong type. Wraps to the space left. A problem that stops something
/// working until it is fixed is a callout, not a status line.
void status(Severity severity, const char* text);

/// A status line with the one thing to do about it at its end: a ghost
/// button led by `action_icon` (an unused value's Remove), the text
/// wrapping short of it. Returns true when it is clicked.
bool status(Severity severity, const char* text, const char* action_icon, const char* action);

/// A problem that stops something working until it is fixed (a shader that
/// failed to compile, a font that failed to bake): a box tinted in the
/// severity's colour with that colour on its left edge, the icon and
/// `title`, then `text` saying what went wrong and what it means now. With
/// an `action`, the button that fixes it, led by `action_icon`; returns true
/// when it is clicked. Everything else is a status line, so a callout always
/// means "this is broken".
bool callout(Severity severity, const char* title, const char* text,
             const char* action_icon = nullptr, const char* action = nullptr);

/// A strip across the top of what it is about, tinted in the severity's
/// colour with a line of it along its bottom: something about all of it
/// that wants an answer, such as a file changed on disk while it has
/// unsaved changes here (sheet 39). The icon and `text`, then a secondary
/// button for each of `answers` at its right end. Returns the index of the
/// answer clicked.
std::optional<std::size_t> notice_strip(Severity severity, const char* text, std::span<const char* const> answers);

/// A line on the raised surface, led by `icon`, saying what the fields under
/// it stand for: "Settings for the 179 picked" over a form that edits
/// several things (sheet 46). `text` is in the secondary colour and `strong`
/// after it in the text colour; on a line too narrow both are cut together.
void note_line(const char* icon, std::string_view text, std::string_view strong = {});

/// The unsaved mark: a small dot in the text colour, centred on `centre`,
/// the same on the tab, the header, the status bar and a footer.
void unsaved_dot(DrawList draw_list, glm::vec2 centre);

/// Its radius, to leave room for it.
inline constexpr float UNSAVED_DOT_RADIUS = 4.0f;

/// A panel with nothing to show: the icon, what the state is ("Nothing
/// selected") and one line of what to do about it, centred in the space
/// left in the window. With an `action`, a button for the one obvious thing
/// to do; returns true when it is clicked. Plain statements, no full stops.
bool empty_state(const char* icon, const char* title, const char* what_to_do,
                 const char* action = nullptr);

} // namespace ftk::ui
