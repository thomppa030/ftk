#include "ftk/ui/kit/table.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ftk::ui {

using namespace corekit;

namespace {

// Room inside each cell, as ImGui's tables give it.
constexpr glm::vec2 CELL_PADDING{4.0f, 2.0f};
// A column is never narrower than this; its edge is grabbed this far
// either side of it.
constexpr float MIN_COLUMN = 8.0f;
constexpr float EDGE_GRAB = 4.0f;

TableState& table() {
    return *current_table();
}

// The columns across the table: the fixed ones as wide as they ask, or as
// their contents came out last frame, those the user dragged as left, and
// the stretched ones sharing what is left by weight, alike or by contents.
void lay_out(TableState& t, float width) {
    const std::size_t count = t.columns.size();
    t.widths.resize(count, 0.0f);
    t.resized.resize(count, false);
    t.content.resize(count, 0.0f);
    float fixed = 0.0f;
    float weights = 0.0f;
    std::vector<float> weight(count, 0.0f);
    for (std::size_t i = 0; i < count; ++i) {
        const Column& column = t.columns[i];
        if (t.resized[i]) {
            fixed += t.widths[i];
        } else if (column.sizing == ColumnWidth::Fixed) {
            t.widths[i] = column.width > 0.0f ? column.width : std::max(t.content[i] + CELL_PADDING.x * 2.0f, MIN_COLUMN);
            fixed += t.widths[i];
        } else {
            weight[i] = t.options.proportional ? std::max(t.content[i], 1.0f) : 1.0f;
            weights += weight[i];
        }
    }
    const float rest = std::max(width - fixed, 0.0f);
    t.x.resize(count);
    float x = t.rect.min.x;
    for (std::size_t i = 0; i < count; ++i) {
        if (weight[i] > 0.0f) t.widths[i] = std::max(rest * weight[i] / weights, MIN_COLUMN);
        t.x[i] = x;
        x += t.widths[i];
    }
}

// The rows go in a region of their own that scrolls under the headers.
void begin_rows(TableState& t) {
    if (!t.options.scroll || t.scrolling) return;
    set_cursor({t.rect.min.x, t.row_bottom});
    ui().begin_region("##rows", {t.rect.width(), core::FILL});
    t.scrolling = true;
    t.row_bottom = cursor().y;
}

void end_cell(TableState& t) {
    if (!t.in_cell) return;
    t.in_cell = false;
    ui().draw_list().pop_clip();
    const glm::vec2 held = ui().layout().end_area();
    const auto column = static_cast<std::size_t>(t.column);
    t.content_next[column] = std::max(t.content_next[column], held.x);
    t.row_bottom = std::max(t.row_bottom, t.row_top + CELL_PADDING.y * 2.0f + held.y);
}

// The row ends: as tall as its tallest cell, and a line at least. Whether
// the mouse is on it is told the next frame, as ImGui tells it.
void end_row(TableState& t) {
    end_cell(t);
    if (t.row < 0) return;
    t.row_bottom = std::max(t.row_bottom, t.row_top + line_height() + CELL_PADDING.y * 2.0f);
    if (mouse_over({t.rect.min.x, t.row_top}, {t.rect.max.x, t.row_bottom})) t.hovered_next = t.row;
}

} // namespace

Table::Table(const char* id, std::span<const Column> columns, TableOptions options) {
    core::Context& context = ui();
    Kit& k = kit();
    const Id table_id = context.id(id);
    TableState& t = k.tables[table_id];
    t.columns = columns;
    t.options = options;
    const glm::vec2 at = cursor();
    t.rect = {.min = at, .max = {at.x + std::max(space_left().x, 1.0f), at.y}};
    // Sorted by the column that asks to be at first, or the first.
    if (options.sortable && t.sort_column < 0 && !columns.empty()) {
        const auto first = std::ranges::find_if(columns, [](const Column& c) { return c.default_sort; });
        t.sort_column = first != columns.end() ? static_cast<int>(first - columns.begin()) : 0;
        t.ascending = true;
        t.sort_dirty = true;
    }
    lay_out(t, t.rect.width());
    t.content_next.assign(columns.size(), 0.0f);
    t.row = -1;
    t.column = -1;
    t.in_cell = false;
    t.scrolling = false;
    t.row_top = at.y;
    t.row_bottom = at.y;
    t.hovered_row = t.hovered_next;
    t.hovered_next = -1;
    context.ids().push(table_id);
    k.table_stack.push_back(table_id);
    open_ = true;
}

Table::~Table() {
    if (!open_) return;
    core::Context& context = ui();
    Kit& k = kit();
    TableState& t = table();
    end_row(t);
    const float top = t.rect.min.y;
    const float bottom = t.row_bottom;
    core::DrawList& dl = context.draw_list();

    // Lines between the columns, and their edges to drag.
    for (std::size_t i = 1; i < t.x.size(); ++i) {
        const float x = t.x[i];
        if (t.options.column_lines) {
            Color line = theme::border();
            line.a *= 0.5f;
            dl.rect_filled({x, top}, {x + 1.0f, bottom}, faded(line));
        }
        if (!t.options.resizable) continue;
        IdScope edge(static_cast<int>(i));
        const core::Interaction in = context.interact(
            context.id("#EDGE"), {.min = {x - EDGE_GRAB, top}, .max = {x + EDGE_GRAB, bottom}}, {.beneath = true});
        if (in.hovered || in.held) set_mouse_cursor(MouseCursor::ResizeEW);
        if (in.pressed) t.resize_from = t.widths[i - 1];
        if (in.held && in.dragging) {
            t.widths[i - 1] = std::max(t.resize_from + (in.position.x - in.press_position.x), MIN_COLUMN);
            t.resized[i - 1] = true;
            context.request_frame();
        }
    }

    t.content = t.content_next;
    if (t.scrolling) {
        // The region holds every row, and is the table's height in what
        // it is in.
        set_cursor({t.rect.min.x, bottom});
        reserve({t.rect.width(), 0.0f});
        context.end_region();
        place_still(context.layout().last_item());
    } else {
        set_cursor(t.rect.min);
        reserve({t.rect.width(), bottom - top});
    }
    context.ids().pop();
    k.table_stack.pop_back();
}

void table_headers() {
    TableState& t = table();
    core::Context& context = ui();
    end_row(t);
    const float height = line_height() + CELL_PADDING.y * 2.0f;
    const float top = t.row_bottom;
    core::DrawList& dl = context.draw_list();
    dl.rect_filled({t.rect.min.x, top}, {t.rect.max.x, top + height}, faded(theme::surface_sunken()));
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        IdScope scope(static_cast<int>(i));
        const core::Rect cell{.min = {t.x[i], top}, .max = {t.x[i] + t.widths[i], top + height}};
        const Id id = context.id(t.columns[i].name);
        const core::Interaction in = place(id, cell);
        context.semantics(id, core::Role::Button, t.columns[i].name, {}, {}, cell);
        if (in.held || in.hovered) {
            dl.rect_filled(cell.min, cell.max, faded(in.held ? theme::surface_active() : theme::surface_hover()));
        }
        // A click sorts by the column; on the column sorted by, the other way.
        if (t.options.sortable && in.clicked) {
            if (t.sort_column == static_cast<int>(i)) {
                t.ascending = !t.ascending;
            } else {
                t.sort_column = static_cast<int>(i);
                t.ascending = true;
            }
            t.sort_dirty = true;
        }
        float right = cell.max.x - CELL_PADDING.x;
        if (t.options.sortable && t.sort_column == static_cast<int>(i)) {
            // The way it sorts, at the cell's right end.
            const float half = std::floor(line_height() * 0.25f);
            const glm::vec2 mid{right - half, top + height * 0.5f};
            const Color ink = faded(theme::text_secondary());
            if (t.ascending) {
                dl.triangle_filled({mid.x - half, mid.y + half * 0.5f}, {mid.x + half, mid.y + half * 0.5f},
                                   {mid.x, mid.y - half * 0.5f}, ink);
            } else {
                dl.triangle_filled({mid.x - half, mid.y - half * 0.5f}, {mid.x, mid.y + half * 0.5f},
                                   {mid.x + half, mid.y - half * 0.5f}, ink);
            }
            right -= half * 2.0f + theme::GAP_S;
        }
        dl.push_clip(cell.min, {right, cell.max.y});
        draw_text(cell.min + CELL_PADDING, corekit::text_colour(), t.columns[i].name);
        dl.pop_clip();
    }
    t.row_bottom = top + height;
    begin_rows(t);
}

void table_next_row() {
    TableState& t = table();
    end_row(t);
    begin_rows(t);
    ++t.row;
    t.column = -1;
    t.row_top = t.row_bottom;
}

bool table_next_column() {
    TableState& t = table();
    end_cell(t);
    if (t.row < 0 || t.column + 1 >= static_cast<int>(t.columns.size())) table_next_row();
    ++t.column;
    const auto column = static_cast<std::size_t>(t.column);
    const float left = t.x[column];
    const float right = left + t.widths[column];
    // What the cell holds goes inside its padding, cut off at its edges.
    core::Context& context = ui();
    const float bottom = std::max(context.layout().area().max.y, t.row_top + CELL_PADDING.y);
    context.draw_list().push_clip({left, t.row_top}, {right, std::numeric_limits<float>::max()});
    context.layout().begin_area({.min = {left + CELL_PADDING.x, t.row_top + CELL_PADDING.y},
                                 .max = {std::max(right - CELL_PADDING.x, left + CELL_PADDING.x), bottom}});
    t.in_cell = true;
    return true;
}

bool table_row_hovered() {
    const TableState* t = current_table();
    return t != nullptr && t->row >= 0 && t->hovered_row == t->row;
}

void table_reveal_row() {
    TableState& t = table();
    if (!t.scrolling || t.in_cell) return;
    core::Context& context = ui();
    // Where the row starts in the region's own space, and the room it takes
    // at the least: a line, or a field's height for one typed in.
    const core::Rect view = context.area_inside();
    const glm::vec2 scroll = context.scroll();
    const float height = std::max(line_height() + CELL_PADDING.y * 2.0f, frame_height());
    const float top = t.row_top - view.min.y + scroll.y;
    if (t.row_top < view.min.y) {
        context.set_scroll({scroll.x, top});
    } else if (t.row_top + height > view.max.y) {
        context.set_scroll({scroll.x, top + height - view.height()});
    }
}

std::optional<TableSort> table_sort_changed() {
    TableState* t = current_table();
    if (t == nullptr || !t->sort_dirty || t->sort_column < 0) return std::nullopt;
    t->sort_dirty = false;
    return TableSort{.column = t->sort_column, .ascending = t->ascending};
}

} // namespace ftk::ui
