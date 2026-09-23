#pragma once

#include <imgui.h>

namespace fjell::ui {

/// What pressing a button does, which decides how it looks. Every button in
/// the editor is one of these; there is one button height.
enum class ButtonKind {
    Secondary,    ///< the default: a raised surface that lightens under the mouse
    Primary,      ///< the one main action of a panel or dialog, in the accent
    Ghost,        ///< low emphasis, for toolbars and rows: no fill until hovered
    Danger,       ///< destroys something that can't be undone, in a confirm dialog
    GhostDanger,  ///< destructive but undoable: quiet until hovered, then red
};

/// A button with text, optionally led by an icon from ui::icon
/// ("Add point", not "Add Point"). A zero size fits the text; a width of -1
/// fills the row. Returns true when clicked.
bool button(const char* label, ButtonKind kind = ButtonKind::Secondary, ImVec2 size = {0.0f, 0.0f});

/// A button that does something to what it sits beside ("Fit to mesh",
/// "Create material"): its icon from ui::icon, then the verb in sentence
/// case. Secondary unless the action destroys something (GhostDanger).
bool action(const char* icon, const char* label, ButtonKind kind = ButtonKind::Secondary);

/// A square button, frame-height tall, showing only an icon. `tooltip`
/// names the action and its shortcut ("Snap to grid (Ctrl G)"): an icon on
/// its own is never enough.
bool icon_button(const char* id, const char* icon, const char* tooltip,
                 ButtonKind kind = ButtonKind::Ghost);

/// A tool or view setting that stays on, drawn as an icon button in the
/// toggle colour while `on`. Returns true when clicked; the caller flips it.
bool toggle_button(const char* id, const char* icon, bool on, const char* tooltip);

/// A tool in a tool row, such as the terrain brush's: a toggle at the tool
/// size (theme::TOOL_BUTTON) with a bigger icon. `tooltip` names the tool and
/// says what it does.
bool tool_button(const char* id, const char* icon, bool on, const char* tooltip);

/// The same toggle with a text label, sized to fit it.
bool toggle(const char* label, bool on, const char* tooltip);

} // namespace fjell::ui
