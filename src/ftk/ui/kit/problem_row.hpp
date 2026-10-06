#pragma once

#include "ftk/ui/kit/feedback.hpp"

#include <string_view>

// One line of a list of what is broken (sheet 15): the severity's icon,
// what is wrong, and where on the right. Like a tree row, the row is the
// last item after it is drawn, for the owner's menu.
//
//     const auto row = ui::problem_row({.id = key.c_str(), .severity = ui::Severity::Error,
//                                       .message = "'foam_bias' : undeclared identifier",
//                                       .meanwhile = "It draws with the last version that compiled",
//                                       .where = "sea.fjsl:41", .selected = picked == i});
//     if (row.clicked) picked = i;
//     if (row.double_clicked) open(problem);
//     if (auto menu = ui::ContextMenu("##problem")) { ... }

namespace ftk::ui {

struct ProblemRowSpec {
    /// Unique among the list's rows; also the row's id.
    const char* id{""};
    Severity severity{Severity::Error};
    /// What is wrong, in one sentence. Cut with an ellipsis when it doesn't
    /// fit, and whole in the tooltip.
    std::string_view message{};
    /// What happens until it is fixed, on a second line in the secondary
    /// colour; empty for none.
    std::string_view meanwhile{};
    /// Where it is ("sea.fjsl:41"), on the right in the mono face.
    std::string_view where{};
    /// Said when the row is hovered, as well as a message cut short: the
    /// file's whole path.
    std::string_view tooltip{};
    bool selected{false};
    /// The search being shown: its match in the message is amber.
    std::string_view query{};
};

struct ProblemRowResult {
    bool clicked{false};
    bool double_clicked{false};
};

/// The problem as a row as wide as the room left, a line tall, or two with
/// what happens meanwhile. Lit under the mouse, in the selection colour
/// while `selected`.
ProblemRowResult problem_row(const ProblemRowSpec& spec);

} // namespace ftk::ui
