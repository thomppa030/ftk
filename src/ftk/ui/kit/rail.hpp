#pragma once

#include "ftk/ui/theme.hpp"

#include <cstddef>
#include <optional>
#include <span>

// A fixed sequence of steps on a vertical line, with the user's items
// placed between them where each belongs. A step is dim text on a dim dot
// and does nothing; an item has its icon in a hue, its label and an
// optional note after it, and selecting it washes it in the selection
// colour. An item may show at more than one place, each of which selects it.
//
//     const ui::RailEntry entries[] = {
//         {.label = "Load"},
//         {.label = "Resize", .icon = icon, .item = 0, .selected = true},
//         {.label = "Save"}};
//     if (const auto item = ui::rail("##steps", entries, theme::Hue::blue)) select(*item);

namespace ftk::ui {

/// One line of a rail(): a step, or an item when it has an `item`.
struct RailEntry {
    const char* label{nullptr};
    /// An item's icon, drawn in the rail's hue.
    const char* icon{nullptr};
    /// What clicking it returns; none for a step.
    std::optional<std::size_t> item{};
    /// Said after the label, in the secondary colour.
    const char* note{nullptr};
    bool selected{false};
};

/// The rail, its items' dots and icons in `hue`. Returns the item clicked,
/// to select or let go; none while nothing is.
std::optional<std::size_t> rail(const char* id, std::span<const RailEntry> entries, theme::Hue hue);

} // namespace ftk::ui
