#pragma once

#include "ftk/ui/draw_list.hpp"

#include <string_view>
#include <utility>

// Property rows: a label column on the left, fitted to the panel, and a
// value column on the right that each field fills. The one row grammar of
// the editor; no label above a field, none to its right, and no field
// whose name is only in a tooltip.
//
//     if (auto t = ui::PropertyTable("##rigidbody")) {
//         ui::row("Mass", [&] { edit |= ui::drag("##mass", rb.mass, {.unit = ui::Unit::Kilograms}); });
//         ui::row("Wind area", "Surface the weather's wind pushes against.",
//                 [&] { edit |= ui::drag("##wind", rb.wind_area, {.unit = ui::Unit::SquareMetres}); });
//         ui::hint("0 leaves the body unmoved by wind");
//     }

namespace ftk::ui {

/// The two-column table rows go into, for as long as it lives.
class PropertyTable {
public:
    explicit PropertyTable(const char* id);
    ~PropertyTable();

    PropertyTable(const PropertyTable&) = delete;
    PropertyTable& operator=(const PropertyTable&) = delete;
    PropertyTable(PropertyTable&&) = delete;
    PropertyTable& operator=(PropertyTable&&) = delete;

    explicit operator bool() const { return open_; }

private:
    bool open_{false};
};

/// While it lives, a search over the rows drawn: a row whose label doesn't
/// contain `query` is left out, the labels that do show the match, headings
/// are skipped with their groups open, and hints are dropped. For a page
/// of settings searched as a whole. An empty query filters nothing.
class RowFilter {
public:
    explicit RowFilter(std::string_view query);
    ~RowFilter();

    RowFilter(const RowFilter&) = delete;
    RowFilter& operator=(const RowFilter&) = delete;
    RowFilter(RowFilter&&) = delete;
    RowFilter& operator=(RowFilter&&) = delete;

    /// How many rows matched so far.
    [[nodiscard]] int matches() const;
};

namespace detail {
/// Starts a row, its label bold when `bold`; false when a RowFilter leaves
/// it out.
bool begin_row(const char* label, const char* help, bool bold = false);
/// Whether a RowFilter with a query is in effect.
[[nodiscard]] bool filtering();
/// After a heading: in a table with no row open, its first or under
/// another heading, the rows after it start under it. Under a row, that
/// row's end takes it in.
void heading_drawn();

/// Where an inherited row's reset goes, taken before its value is drawn.
struct InheritedValue {
    float top;
    float right;
};
/// Narrows the value to leave the reset's room.
[[nodiscard]] InheritedValue begin_inherited_value();
/// The reset after the value, or its room; true when it was clicked.
bool end_inherited_value(const InheritedValue& value, bool set_here, const char* reset_tip);
/// A line under the row above, `glyph` before it when there is one, both
/// in `colour`: a row of its own running under both columns while a
/// property table is open, at the cursor where none is. What hint() and a
/// status line in a table draw.
void line_under_row(const char* glyph, Color colour, const char* text);
} // namespace detail

/// One row: `label` on the left, then `draw_value` in the value column with
/// the next item's width set to fill it.
template <typename F>
void row(const char* label, F&& draw_value) {
    if (!detail::begin_row(label, nullptr)) return;
    std::forward<F>(draw_value)();
}

/// A row whose label is followed by the help icon; hovering it shows
/// `help`. For explanations longer than a hint.
template <typename F>
void row(const char* label, const char* help, F&& draw_value) {
    if (!detail::begin_row(label, help)) return;
    std::forward<F>(draw_value)();
}

/// A row whose value is either set on the thing being edited or comes from
/// what it inherits from, a parent material or a shader's default (sheet 37,
/// I1). Set here, its label is bold and a reset at the row's end takes the
/// value back, `reset_tip` saying to what; inherited, it looks like any other
/// row. The value leaves the reset's room either way, so the values of a
/// table line up. Returns true when the reset was clicked.
template <typename F>
bool inherited_row(const char* label, bool set_here, const char* reset_tip, F&& draw_value) {
    if (!detail::begin_row(label, nullptr, set_here)) return false;
    const detail::InheritedValue value = detail::begin_inherited_value();
    std::forward<F>(draw_value)();
    return detail::end_inherited_value(value, set_here, reset_tip);
}

/// The next row's label leads with an amber dot when `set_here`: its value
/// is set on the thing being edited rather than coming from elsewhere (a
/// style rule, the parent, a default). Unmarked rows of the same table call
/// it with false, which keeps the dot's room so every label lines up.
void mark_next_row(bool set_here);

/// One short line under the row above, in the secondary colour: what you
/// need to know while editing that field. Anything longer is help.
void hint(const char* text);

} // namespace ftk::ui
