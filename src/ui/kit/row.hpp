#pragma once

#include <imgui.h>

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

namespace detail {
void begin_row(const char* label, const char* help);
} // namespace detail

/// One row: `label` on the left, then `draw_value` in the value column with
/// the next item's width set to fill it.
template <typename F>
void row(const char* label, F&& draw_value) {
    detail::begin_row(label, nullptr);
    std::forward<F>(draw_value)();
}

/// A row whose label is followed by the help icon; hovering it shows
/// `help`. For explanations longer than a hint.
template <typename F>
void row(const char* label, const char* help, F&& draw_value) {
    detail::begin_row(label, help);
    std::forward<F>(draw_value)();
}

/// One short line under the row above, in the secondary colour: what you
/// need to know while editing that field. Anything longer is help.
void hint(const char* text);

} // namespace fjell::ui
