#pragma once

#include "ftk/ui/theme.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// One of a list whose rows are ticked for what is done to them and clicked
// to be shown (sheet 46), as models waiting to be imported are: the tick's
// box at the left, the kind's icon in its colour, the name, and at the right
// either a figure in the small mono face or how the work on it stands. While
// the work waits the row is faint; while it runs a spinner turns and a thin
// accent bar along the row's foot fills; once done it shows the success
// mark, once failed the error mark. The row shown is washed in the selection
// colour, the one under the mouse lit. Like a tree row, the row is the last
// item after it is drawn, for the owner's menu.
//
//     const auto row = ui::pick_row({.id = key.c_str(), .picked = model.picked, .icon = icon::mesh,
//                                    .hue = theme::Hue::blue, .name = "crate.gltf", .figure = "264"},
//                                   shown == i);
//     if (row.toggled) model.picked = !model.picked;
//     if (row.clicked) shown = i;

namespace ftk::ui {

/// How the work on a row stands.
enum class PickRowWork : uint8_t { none, waiting, running, done, failed };

struct PickRowSpec {
    /// Unique among the list's rows; also the row's id.
    const char* id{""};
    /// Ticked.
    bool picked{false};
    /// The tick can't be changed now: while the work runs.
    bool locked{false};
    /// The kind's icon, in `hue` (the secondary text colour without one).
    const char* icon{nullptr};
    std::optional<theme::Hue> hue{};
    std::string_view name{};
    /// Shown at the right while there is no work to show: a count, a size.
    std::string_view figure{};
    PickRowWork work{PickRowWork::none};
    /// The share of the running work done, 0 to 1.
    float progress{0.0f};
    /// Said when the row is hovered, after a name cut short: the whole
    /// path, why the work failed.
    std::string_view tooltip{};
    /// The search being shown: its match in the name is amber.
    std::string_view query{};
};

struct PickRowResult {
    /// The row was clicked, away from its box.
    bool clicked{false};
    /// Its box was clicked: the caller flips the tick.
    bool toggled{false};
    bool double_clicked{false};
};

/// A row as wide as the space left, a tree row tall, washed while
/// `selected`. Out of sight it keeps its room and draws nothing.
PickRowResult pick_row(const PickRowSpec& spec, bool selected);

} // namespace ftk::ui
