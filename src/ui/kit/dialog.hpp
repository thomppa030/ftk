#pragma once

#include <imgui.h>

#include <utility>

// A dialog that asks before something that can't be undone: the editor
// behind it dims, the question is the title, the caller draws what the
// answer depends on, and the buttons sit on the right, the verb on the
// confirming one ("Delete 3 files", never "OK"). Anything that can be undone
// happens without asking.
//
//     if (want_to_delete) ui::open_dialog("##delete");
//     const auto answer = ui::confirm_dialog("##delete", {.title = "Delete crate.fjmesh?",
//                                            .confirm = "Delete file", .destructive = true},
//                                            [&] { ui::status(ui::Severity::Warning, "Used by 2 files"); });
//     if (answer == ui::DialogAnswer::Confirm) delete_it();

namespace fjell::ui {

enum class DialogAnswer { None, Confirm, Cancel };

struct DialogSpec {
    const char* title{""};
    /// The verb on the confirming button.
    const char* confirm{"OK"};
    /// Destroys something: the confirming button is the danger kind and
    /// Cancel starts with the focus, so Enter cancels until the user moves.
    bool destructive{false};
};

/// Opens the dialog `id` on its next draw.
void open_dialog(const char* id);

namespace detail {
bool begin_dialog(const char* id, const DialogSpec& spec);
DialogAnswer end_dialog(const DialogSpec& spec);
} // namespace detail

/// Draws the dialog `id` while it is open, `body` between its title and its
/// buttons. Returns the answer on the frame it is given; Esc cancels.
template <typename Body>
DialogAnswer confirm_dialog(const char* id, const DialogSpec& spec, Body&& body) {
    if (!detail::begin_dialog(id, spec)) return DialogAnswer::None;
    std::forward<Body>(body)();
    return detail::end_dialog(spec);
}

} // namespace fjell::ui
