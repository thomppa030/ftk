#pragma once

#include <imgui.h>

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

namespace fjell::ui {

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
/// Starts a row; false when a RowFilter leaves it out.
bool begin_row(const char* label, const char* help);
/// Whether a RowFilter with a query is in effect.
[[nodiscard]] bool filtering();
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

/// One short line under the row above, in the secondary colour: what you
/// need to know while editing that field. Anything longer is help.
void hint(const char* text);

} // namespace fjell::ui
