#pragma once

#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>

// A dialog that asks before something that can't be undone: the editor
// behind it dims, the question is the title, the caller draws what the
// answer depends on, and the buttons sit on the right, the verb on the
// confirming one ("Delete 3 files", never "OK"). Anything that can be undone
// happens without asking.
//
//     if (want_to_delete) ui::open_dialog("##delete");
//     const auto answer = ui::confirm_dialog("##delete", {.title = "Delete crate.png?",
//                                            .confirm = "Delete file", .destructive = true},
//                                            [&] { ui::status(ui::Severity::Warning, "Used by 2 files"); });
//     if (answer == ui::DialogAnswer::Confirm) delete_it();

namespace ftk::ui {

enum class DialogAnswer { None, Confirm, Cancel };

struct DialogSpec {
    const char* title{""};
    /// The verb on the confirming button.
    const char* confirm{"OK"};
    /// The word on the button that answers no, which Esc presses: "Not now"
    /// on a dialog that asks again later, "Close" once nothing is left to do.
    const char* cancel{"Cancel"};
    /// Destroys something: the confirming button is the danger kind and
    /// Cancel starts with the focus, so Enter cancels until the user moves.
    bool destructive{false};
};

/// A dialog that asks for a name before making something ("Save layout
/// as", "New script").
struct PromptSpec {
    const char* title{""};
    /// Shown in the empty field, saying what the name is for.
    const char* hint{""};
    /// The verb on the confirming button.
    const char* confirm{"OK"};
};

/// Where a dialog hangs (sheet 18): the middle of its top edge, or its left
/// end with `align` 0, flush with the bottom of the surface that asked.
struct DialogAnchor {
    glm::vec2 at{};
    float align{0.5f};
};

/// Opens the dialog `id` on its next draw, hanging from under the editor
/// header: a question about the asset in the tab, or about the editor.
void open_dialog(const char* id);

/// Opens the dialog `id` hanging from `anchor`: from the content browser's
/// bar for a file there, from under the menu bar at its menu.
void open_dialog(const char* id, DialogAnchor anchor);

/// Hanging from the bottom edge of the window called `window`, centred
/// across it; none when that window isn't there.
[[nodiscard]] std::optional<DialogAnchor> anchor_under(const char* window);

namespace detail {
/// How a dialog's buttons behave, from what its body holds.
struct DialogButtons {
    /// False greys the confirming button out, with `why_not` as its tooltip.
    bool can_confirm{true};
    const char* why_not{nullptr};
    /// The confirming button takes the focus as the dialog opens: for a
    /// body with nothing to type into.
    bool focus_confirm{true};
    /// The body confirmed this frame (Enter in its field).
    bool confirmed{false};
};

bool begin_dialog(const char* id, const DialogSpec& spec);
/// Ends the dialog with its buttons; `left`, when given, draws first on
/// their line in the room left of them, given that room's width (a status
/// line).
DialogAnswer end_dialog(const DialogSpec& spec, const DialogButtons& buttons = {},
                        const std::function<void(float width)>& left = {});
/// Cancel, then the verb, at the right of the current line, `inset` in
/// from the window's content edge.
DialogAnswer dialog_buttons(const DialogSpec& spec, const DialogButtons& buttons, bool appearing, float inset = 0.0f);
} // namespace detail

/// The bar along the bottom of a standalone window (sheet 9): a rule across
/// it, `left` drawn first in the space left of the buttons (a name field, a
/// status line) given that space's width, then Cancel and the verb. Enter
/// confirms and Esc cancels unless something is being typed.
DialogAnswer window_bar(const DialogSpec& spec, const detail::DialogButtons& buttons,
                        const std::function<void(float width)>& left = {});

/// Draws the dialog `id` while it is open, `body` between its title and its
/// buttons. Returns the answer on the frame it is given; Esc cancels.
template <typename Body>
DialogAnswer confirm_dialog(const char* id, const DialogSpec& spec, Body&& body) {
    if (!detail::begin_dialog(id, spec)) return DialogAnswer::None;
    std::forward<Body>(body)();
    return detail::end_dialog(spec);
}

/// Asks for a name in a field that has the focus as the dialog opens.
/// Enter confirms and Esc cancels; the confirming button is greyed out
/// while the name is empty. `text` is the field's text, kept by the caller
/// and emptied before opening. Returns the answer on the frame it is given.
DialogAnswer prompt_dialog(const char* id, const PromptSpec& spec, std::string& text);

/// What to do with unsaved changes before something closes.
enum class UnsavedAnswer { None, Save, DontSave, Cancel };

/// Asks before closing one thing with unsaved changes (sheet 7): `title`
/// ("Save changes to crate_wood.json?"), `text` saying what is lost, then
/// Don't save set apart on the left, Cancel, and Save with the focus.
/// Enter saves, Esc cancels. Returns the answer on the frame it is given.
UnsavedAnswer unsaved_dialog(const char* id, const char* title, const char* text);

/// One unsaved thing in the list quitting asks about.
struct UnsavedItem {
    const char* icon{nullptr};
    Color icon_colour{};
    std::string name{};    ///< "crate_wood.json"
    std::string detail{};  ///< what else saving it writes, dimmed after the name
    bool save{true};       ///< ticked: saved when the answer is Save
};

/// Asks before quitting with several unsaved things: one list, a checkbox
/// each, Don't save on the left, Cancel, and "Save N and quit" counting the
/// ticked ones. Returns the answer on the frame it is given.
UnsavedAnswer unsaved_list_dialog(const char* id, const char* title, std::span<UnsavedItem> items);

/// How a name a copy finds already there is answered.
enum class ClashChoice : std::uint8_t { Replace, KeepBoth, Skip };

/// A name a copy finds already there, which the clash dialog asks about.
struct ClashItem {
    const char* icon{nullptr};
    Color icon_colour{};
    std::string name{};     ///< "crate.gltf"
    std::string here{};     ///< what is there: "398 KB, 2 Oct 14:20"
    std::string dropped{};  ///< what comes: "412 KB, today 09:12"
    /// Said beside the name while it is replaced: what else that changes
    /// ("crate.fjmesh comes back changed"); empty for nothing.
    std::string replace_note{};
    /// The name a copy kept beside it takes ("lantern_1.glb"), said beside
    /// the name while Keep both is chosen.
    std::string kept_name{};
    /// What is there is a file the copy can write over; Replace isn't
    /// offered for a folder.
    bool replaceable{true};
    ClashChoice choice{ClashChoice::Skip};
};

/// What the clash dialog says around its rows.
struct ClashSpec {
    /// The question: "Replace the 3 files already in props?"
    const char* title{""};
    /// What was copied without asking, in a success line ("9 other files
    /// were copied."); empty for nothing.
    std::string copied{};
    /// The folder copied into, as the warning names it ("props").
    std::string folder{};
};

/// Opens the clash dialog `id` hanging from `anchor`, as wide as it is.
void open_clash_dialog(const char* id, DialogAnchor anchor);
/// The same hanging from under the editor header, as open_dialog() does.
void open_clash_dialog(const char* id);
/// Asks about names a copy finds already there (sheet 46, D1): the title,
/// what was copied already, a row "Every file" whose Replace | Keep both |
/// Skip sets them all, then a row per name on the sunken surface with what
/// is there and what comes in the mono face and its own choice, which the
/// dialog changes in `items`. The verb counts what will be copied ("Copy 2
/// files"); while anything is replaced it is the danger kind, a warning in
/// the bar names what is written over, and Cancel takes Enter. Cancel and
/// Esc answer Cancel, which the caller takes as Skip for every one. Returns
/// the answer on the frame it is given.
DialogAnswer clash_dialog(const char* id, const ClashSpec& spec, std::span<ClashItem> items);

/// What to do with the unsaved work a program kept when it last ended
/// without saving it.
enum class RecoverAnswer { None, Recover, Discard, Later };

/// Asks, as a program starts, about the unsaved work it kept when it last
/// ended (sheet 39): `title`, `text` saying what was kept, then the kept
/// things in a list, a checkbox each (an item's `save` ticks it to be
/// recovered), Discard set apart on the left and "Recover N files" counting
/// the ticked ones. Enter recovers; Esc answers Later, to be asked again at
/// the next start. Returns the answer on the frame it is given.
RecoverAnswer recover_dialog(const char* id, const char* title, const char* text, std::span<UnsavedItem> items);

} // namespace ftk::ui
