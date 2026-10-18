#include "ftk/ui/kit/row.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ftk::ui {

using namespace corekit;

namespace {

// Inside a row, above and below its tallest cell; and at the table's sides.
// ImGui's table cell padding.
constexpr glm::vec2 CELL_PADDING{4.0f, 2.0f};

// The help icon after a label: dim until the mouse is on it, the
// explanation in a tooltip.
void help_icon(const char* help) {
    const glm::vec2 at = cursor();
    const glm::vec2 size{measure(icon::help).x, frame_height()};
    const bool hovered = mouse_over(at, at + size) && ui().area_or_inner_under_pointer();
    align_text_to_frame();
    ui::text(icon::help, hovered ? theme::text() : theme::text_disabled());
    item_tooltip(help);
}

// The dot, or the room it takes, before a label.
void draw_mark(RowMark mark) {
    constexpr float DOT = 6.0f;
    const glm::vec2 at = cursor();
    if (mark == RowMark::Dot) {
        const float cy = at.y + frame_height() * 0.5f;
        ui().draw_list().circle_filled({at.x + DOT * 0.5f, cy}, DOT * 0.5f, faded(theme::accent()));
    }
    reserve({DOT, frame_height()});
    same_line(theme::GAP_S + theme::GAP_XS);
}

// Where a row's height is kept, the room it keeps while out of sight.
Id height_id(Id row) {
    return core::hash_label("##row_height", row);
}

// The row being drawn ends: the next goes under its tallest cell.
void end_row() {
    Kit& k = kit();
    PropertyRows& rows = k.rows;
    if (!rows.in_row) return;
    const float value_bottom = cursor().y - k.style.item_spacing.y;
    rows.bottom = std::max(rows.label_bottom, value_bottom) + CELL_PADDING.y;
    rows.in_row = false;
    if (rows.id != 0) store_float(height_id(rows.id), rows.bottom - rows.row_top);
    k.row_drawn = 0;
}

// A row out of sight, as tall as it was when last drawn: its room is taken,
// the table and what scrolls it as tall as when it is drawn, and nothing
// else. A row never drawn yet is drawn, to learn its height.
bool skip_row(Kit& k, Id row) {
    PropertyRows& rows = k.rows;
    const float height = stored_float(height_id(row), 0.0f);
    if (height <= 0.0f) return false;
    const core::Rect rect{.min = {rows.left, rows.bottom}, .max = {rows.right, rows.bottom + height}};
    if (!can_skip_row(row, rect)) return false;
    // What a drawn row's cells cover, inside its padding.
    set_cursor({rect.min.x, rect.min.y + CELL_PADDING.y});
    place_still(ui().layout().next({rect.width(), std::max(height - CELL_PADDING.y * 2.0f, 0.0f)}));
    rows.bottom = rect.max.y;
    return true;
}

} // namespace

RowFilter::RowFilter(std::string_view query) {
    kit().row_search = {.active = !query.empty(), .query = std::string(query), .matches = 0};
}

RowFilter::~RowFilter() {
    kit().row_search = {};
}

int RowFilter::matches() const {
    return kit().row_search.matches;
}

bool detail::filtering() {
    return kit().row_search.active;
}

void detail::heading_drawn() {
    Kit& k = kit();
    PropertyRows& rows = k.rows;
    if (!rows.open || rows.in_row) return;
    // Spaced as a row's end would be.
    rows.bottom = cursor().y - k.style.item_spacing.y + CELL_PADDING.y;
}

PropertyTable::PropertyTable(const char* id) {
    core::Context& context = ui();
    const glm::vec2 at = cursor();
    const float room = space_left().x;
    // A label column of the theme's share, the value filling the rest, both
    // inside the table's padding at its sides.
    kit().rows = {.open = true,
                  .left = at.x + CELL_PADDING.x,
                  .value_x = at.x + theme::label_column(room) + CELL_PADDING.x,
                  .right = at.x + room - CELL_PADDING.x,
                  .bottom = at.y};
    context.ids().push(id);
    open_ = true;
}

PropertyTable::~PropertyTable() {
    end_row();
    PropertyRows& rows = kit().rows;
    if (open_) {
        ui().ids().pop();
        // What follows goes under the table.
        set_cursor({rows.left - CELL_PADDING.x, rows.bottom});
        rows = {};
    }
    detail::set_edit_label(nullptr);
}

bool detail::begin_row(const char* label, const char* help, bool bold) {
    Kit& k = kit();
    // Taken here so a row a search leaves out doesn't hand its mark on.
    const RowMark mark = std::exchange(k.row_mark, RowMark::None);
    RowSearch& search = k.row_search;
    if (search.active && !matches(label, search.query)) return false;
    set_edit_label(label);
    end_row();
    // Under a search every match is drawn, to be counted.
    const Id row = ui().id(label);
    if (!search.active && skip_row(k, row)) return false;
    PropertyRows& rows = k.rows;
    rows.in_row = true;
    rows.id = row;
    k.row_drawn = row;
    rows.row_top = rows.bottom;
    const float top = rows.row_top + CELL_PADDING.y;
    set_cursor({rows.left, top});
    if (mark != RowMark::None) draw_mark(mark);
    {
        FontScope face(bold ? Font::Bold : Font::Current, 0.0f);
        align_text_to_frame();
        if (search.active) {
            ++search.matches;
            highlighted_text(label, search.query);
        } else {
            text(label);
        }
    }
    if (help != nullptr) {
        same_line(theme::GAP_S);
        help_icon(help);
    }
    rows.label_bottom = top + frame_height();
    // The value goes in its column, as wide as the column is.
    const Item label_item = k.item;
    set_cursor({rows.value_x, top});
    k.next.width = rows.right - rows.value_x;
    // In the value column the last item is still the row's label.
    k.item = label_item;
    return true;
}

detail::InheritedValue detail::begin_inherited_value() {
    Kit& k = kit();
    const float room = frame_height() + theme::GAP_S;
    const glm::vec2 start = cursor();
    // In a table the value's column ends a cell's padding short of the edge.
    const float edge = start.x + space_left().x;
    const float right = k.rows.in_row ? std::min(k.rows.right, edge) : edge;
    const InheritedValue value{.top = start.y, .right = right};
    // Every item of the value, not only its first, stops short of the reset.
    const float short_of_edge = room + edge - right;
    k.next.width = -short_of_edge;
    k.item_widths.push_back(-short_of_edge);
    return value;
}

bool detail::end_inherited_value(const InheritedValue& value, bool set_here, const char* reset_tip) {
    kit().item_widths.pop_back();
    const float size = frame_height();
    same_line();
    set_cursor({value.right - size, value.top});
    if (!set_here) {
        reserve({size, size});
        return false;
    }
    return icon_button("##reset", icon::revert, reset_tip);
}

void mark_next_row(bool set_here) {
    kit().row_mark = set_here ? RowMark::Dot : RowMark::Room;
}

void detail::line_under_row(const char* glyph, Color colour, const char* text) {
    Kit& k = kit();
    FontScope small(Font::Current, theme::SMALL_TEXT);
    PropertyRows& rows = k.rows;
    float width = space_left().x;
    if (rows.open) {
        // A row of its own that runs under both columns, from the label's
        // edge to the value's.
        end_row();
        rows.in_row = true;
        // Drawn always, with no label to keep a height by.
        rows.id = 0;
        rows.row_top = rows.bottom;
        width = rows.right - rows.left;
        set_cursor({rows.left, rows.row_top + CELL_PADDING.y});
    }
    // The glyph, then the text wrapping in the room after it.
    const float indent = glyph != nullptr ? measure(glyph).x + theme::GAP_S + theme::GAP_XS : 0.0f;
    const float wrap = std::max(width - indent, 0.0f);
    const core::Rect placed = ui().layout().next({width, measure(text, wrap).y});
    if (glyph != nullptr) draw_text(placed.min, colour, glyph);
    draw_text({placed.min.x + indent, placed.min.y}, colour, text, wrap);
    place_still(placed);
    if (rows.open) rows.label_bottom = placed.max.y;
}

void hint(const char* text) {
    // A hint belongs to the row above, which a search may have left out.
    if (detail::filtering()) return;
    detail::line_under_row(nullptr, theme::text_secondary(), text);
}

} // namespace ftk::ui
