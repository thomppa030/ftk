#pragma once

// A field of chips: what a thing holds of a set of names, one chip each, the
// body text on a neutral grey, wrapping onto more lines as they need. The
// field is a combo box, lit on hover as one is, a chevron at its end and a
// dim "None" while it holds nothing: a click anywhere on it opens what
// chooses its names (a tree picker, say), which the caller answers. A
// hovered chip's right end turns into an ×, which takes it off. A chip
// naming something that isn't there is drawn in the error colour, its
// tooltip saying why.
//
//     const ui::ChipEdit edit = ui::chip_field("##tags", chips, "Choose tags");
//     if (edit.removed) take_off(*edit.removed);
//     if (edit.open) ui::tree_picker::open("##tags_picker", *edit.open);

#include <glm/vec2.hpp>

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
    /// The field was clicked off every ×: where what it opens goes, under
    /// the field's left edge as a combo box's list.
    std::optional<glm::vec2> open{};
};

/// The field across the rest of the row, `chips` in order. `label` names it
/// for what reads the screen: what choosing in it does ("Choose tags").
ChipEdit chip_field(const char* id, std::span<const Chip> chips, const char* label);

} // namespace ftk::ui
