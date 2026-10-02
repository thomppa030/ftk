#pragma once

#include <cstddef>
#include <optional>
#include <span>

// A field of chips: values from a short list that a thing carries, such as a
// node's marks (sheet 37, M1). Each chip has a remove; the field ends in a
// chevron, and clicking it opens a picker of the whole list, each entry with
// a box saying whether the thing carries it and a note at its right. A chip
// that comes from elsewhere (a parent's) is outlined and led by the
// inherited icon, with no remove: it is taken off where it is set.
//
//     const ui::ChipFieldEdit chips = ui::chip_field("##marks", carried, options, &edit_marks);
//     if (chips.removed) take_off(carried_bits[*chips.removed]);
//     if (chips.toggled) toggle(*chips.toggled);
//     if (chips.footer) settings.show_marks();

namespace ftk::ui {

/// One chip of a chip_field().
struct Chip {
    const char* label{""};
    /// It comes from elsewhere: outlined, led by the inherited icon, and
    /// with no remove.
    bool inherited{false};
    /// Shown while the chip is under the mouse; for an inherited chip, where
    /// it comes from.
    const char* tooltip{nullptr};
};

/// One entry of a chip field's picker: a value the thing carries or not,
/// with a note at the right ("14 nodes").
struct ChipOption {
    const char* label{""};
    bool on{false};
    const char* note{nullptr};
};

/// The entry under a line at the picker's end, which leaves it to edit the
/// list itself ("Edit marks…").
struct ChipFooter {
    const char* icon{nullptr};
    const char* label{""};
};

struct ChipFieldEdit {
    /// The chip whose remove was clicked.
    std::optional<std::size_t> removed;
    /// The picker entry clicked, to be turned on or off. The picker stays
    /// open, so several are set in one go.
    std::optional<std::size_t> toggled;
    /// The footer was chosen; the picker has closed.
    bool footer{false};
};

/// The field, filling the width it is given, its chips wrapping onto more
/// lines as they need; `options` fill its picker, `footer` (null for none)
/// ends it.
ChipFieldEdit chip_field(const char* id, std::span<const Chip> chips, std::span<const ChipOption> options,
                         const ChipFooter* footer = nullptr);

} // namespace ftk::ui
