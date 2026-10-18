#pragma once

// A field of chips: what a thing holds of a set of names, one chip each, the
// body text on a neutral grey, wrapping onto more lines as they need, with a
// + at the field's end that the caller answers by opening what adds one (a
// tree picker, say). A hovered chip's right end turns into an ×, which takes
// it off. A chip naming something that isn't there is drawn in the error
// colour, its tooltip saying why.
//
//     const ui::ChipEdit edit = ui::chip_field("##tags", chips, "Add a tag");
//     if (edit.removed) take_off(*edit.removed);
//     if (edit.add) ui::tree_picker::open("##tags");

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace ftk::ui {

struct Chip {
    std::string_view text;
    /// Why the chip names nothing that is there, its tooltip; empty for a
    /// chip that does.
    std::string_view problem{};
};

/// What the field did this frame.
struct ChipEdit {
    /// The chip whose × was clicked.
    std::optional<std::size_t> removed{};
    /// The + was clicked.
    bool add{false};
};

/// The field across the rest of the row, `chips` in order. `add_tooltip`
/// says what the + adds.
ChipEdit chip_field(const char* id, std::span<const Chip> chips, const char* add_tooltip = "Add");

} // namespace ftk::ui
