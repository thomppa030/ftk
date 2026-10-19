#include "ftk/ui/kit/tree.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/core/text_input.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/search.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace ftk::ui {

using namespace corekit;

namespace {

// Room at the row's ends, the chevron's column, the gap between the row's
// parts, the dots and the gap between them, and the corner.
constexpr float ROW_PAD = 4.0f;
constexpr float TWISTY = 14.0f;
constexpr float PART_GAP = 6.0f;
constexpr float DOT = 7.0f;
constexpr float DOT_GAP = 4.0f;
constexpr float DOTS_LEAD = 8.0f;
constexpr float ROUNDING = 3.0f;
// The insert line's thickness and the ring at its start.
constexpr float INSERT_LINE = 2.0f;
constexpr float RING = 8.0f;

// The tree's metrics are drawn for the text size around it, and scale with
// the size its rows are drawn at.
float scale() {
    const float base = kit().tree_base_size;
    return base > 0.0f ? font_size() / base : 1.0f;
}
float row_height() { return std::round(theme::TREE_ROW * scale()); }
float indent() { return std::round(theme::TREE_INDENT * scale()); }
float twisty() { return std::round(TWISTY * scale()); }

} // namespace

void RenameBox::start(std::uint32_t key, std::string_view name) {
    key_ = key;
    original_ = std::string(name);
    text_ = original_;
    focus_ = true;
}

std::optional<std::string> RenameBox::draw(float width) {
    core::Context& context = ui();
    const Id field = context.id("##rename");
    field_ = field;
    if (focus_) {
        context.set_focus(field);
        focus_ = false;
    }
    const core::Rect rect = context.layout().next({width, frame_height()});
    // Esc puts the text back as it was when the field was entered, so the
    // edit ends with the old name and changes nothing.
    const TextInput in = text_input(field, rect, text_, {.select_all = true});
    context.semantics(field, core::Role::TextField, "Name", text_);
    // A sunken field edged in amber: the name being edited.
    context.draw_list().rect(rect.min, rect.max, faded(theme::accent()), kit().style.frame_rounding);
    if (!in.deactivated) return std::nullopt;
    key_.reset();
    if (text_.empty() || text_ == original_) return std::nullopt;
    return text_;
}

std::optional<std::string> RenameBox::finish() {
    if (!key_) return std::nullopt;
    key_.reset();
    // The field lets go of the keyboard as Enter would have it, keeping
    // nothing for the next name started in it.
    Kit& k = kit();
    if (k.editing.id == field_) k.editing = {};
    if (k.left.id == field_) k.left = {};
    if (ui().focused() == field_) ui().clear_focus();
    if (text_.empty() || text_ == original_) return std::nullopt;
    return text_;
}

Tree::Tree(const char* id, float text_size) : sized_{text_size > 0.0f} {
    ui().ids().push(id);
    // Rows sit edge to edge.
    Style style = kit().style;
    style.item_spacing.y = 0.0f;
    push_style(style);
    if (sized_) {
        outer_size_ = std::exchange(kit().tree_base_size, font_size());
        core::TextStyle text = ui().text().style();
        text.size = text_size;
        ui().text().push_style(text);
    }
}

Tree::~Tree() {
    if (sized_) {
        ui().text().pop_style();
        kit().tree_base_size = outer_size_;
    }
    pop_style();
    ui().ids().pop();
}

TreeRowResult tree_row(const TreeRowSpec& spec) {
    TreeRowResult result;
    core::Context& context = ui();
    IdScope scope(spec.id);
    const Id open_id = context.id("##open");
    bool open = spec.has_children && stored_bool(open_id, spec.starts_open);

    const glm::vec2 min = cursor();
    const float width = std::max(space_left().x, 1.0f);
    const glm::vec2 max{min.x + width, min.y + row_height()};
    const bool renaming = spec.rename != nullptr && spec.rename->editing(spec.key);
    const Id row_id = context.id("##row");

    // Out of sight a row keeps its room and its open state, and draws
    // nothing: a long tree costs what shows of it.
    if (!renaming && can_skip_row(row_id, {.min = min, .max = max})) {
        place_still(context.layout().next({width, row_height()}));
        result.open = spec.has_children && (open || spec.force_open);
        return result;
    }
    Kit& k = kit();
    k.row_drawn = row_id;

    // The whole row is the click target. Only the rename field may sit over
    // it: allowing overlap otherwise would let something drawn behind the
    // tree take the row's mouse.
    if (renaming) next_item_allows_overlap();
    const core::Rect row = context.layout().next({width, row_height()});
    const core::Interaction in = place(row_id, row);
    context.semantics(row_id, core::Role::TreeItem, spec.name, {},
                      {.selected = spec.selection != RowSelection::None, .expanded = open || spec.force_open});
    const bool hovered = in.hovered;
    const float twisty_x = min.x + ROW_PAD + static_cast<float>(spec.depth) * indent();
    if (in.clicked) {
        const float pointer = in.press_position.x;
        const bool on_chevron = pointer >= twisty_x && pointer < twisty_x + twisty();
        if (spec.has_children && (on_chevron || spec.fold_on_click)) {
            open = !open;
            store_bool(open_id, open);
        }
        result.clicked = !(spec.has_children && on_chevron);
    }
    result.double_clicked = hovered && in.double_clicked;

    core::DrawList& dl = context.draw_list();
    if (spec.selection == RowSelection::Primary) {
        dl.rect_filled(min, max, faded(theme::selection()), ROUNDING);
    } else if (spec.selection == RowSelection::Secondary) {
        dl.rect_filled(min, max, faded(theme::selection_secondary()), ROUNDING);
    } else if (hovered) {
        dl.rect_filled(min, max, faded(theme::surface_hover()), ROUNDING);
    }

    // A guide down through each level above, under its parent's chevron.
    for (int level = 0; level < spec.depth; ++level) {
        const float x = std::floor(min.x + ROW_PAD + static_cast<float>(level) * indent() + twisty() * 0.5f);
        dl.rect_filled({x, min.y}, {x + 1.0f, max.y}, faded(theme::surface_active()));
    }

    const float text_y = std::floor(min.y + (row_height() - font_size()) * 0.5f);
    if (spec.has_children) {
        const char* chevron = open || spec.force_open ? icon::fold_open : icon::fold_closed;
        draw_text({twisty_x + (twisty() - measure(chevron).x) * 0.5f, text_y}, theme::text_disabled(), chevron);
    }

    float x = twisty_x + twisty() + PART_GAP;
    if (spec.icon != nullptr) {
        const Color tint = spec.tint.value_or(spec.hue ? theme::hue(*spec.hue) : theme::text_secondary());
        draw_text({x, text_y}, tint, spec.icon);
        x += measure(spec.icon).x + PART_GAP;
    }

    // The dots at the right end, one per hue.
    const float dots_w = spec.dots.empty() ? 0.0f
                                           : DOTS_LEAD + static_cast<float>(spec.dots.size()) * DOT +
                                                 static_cast<float>(spec.dots.size() - 1) * DOT_GAP;
    float dot_x = max.x - ROW_PAD - dots_w + DOTS_LEAD;
    for (theme::Hue hue : spec.dots) {
        dl.circle_filled({dot_x + DOT * 0.5f, (min.y + max.y) * 0.5f}, DOT * 0.5f, faded(theme::hue(hue)));
        dot_x += DOT + DOT_GAP;
    }

    // The tag before the dots, at the small size, centred on the row's text.
    float tag_w = 0.0f;
    if (!spec.tag.empty()) {
        const float row_text = font_size();
        FontScope small(Font::Current, row_text * theme::SMALL_TEXT / theme::BODY_TEXT);
        const float w = measure(spec.tag).x;
        draw_text({max.x - ROW_PAD - dots_w - w, std::floor(text_y + (row_text - font_size()) * 0.5f)},
                  theme::text_disabled(), spec.tag);
        tag_w = w + PART_GAP;
    }

    const float name_right = max.x - ROW_PAD - dots_w - tag_w;
    const Color ink = spec.dimmed ? theme::text_disabled() : theme::text();
    if (renaming) {
        set_cursor({x - kit().style.frame_padding.x, min.y + (row_height() - frame_height()) * 0.5f});
        result.renamed = spec.rename->draw(std::max(name_right - x, 1.0f));
    } else if (x + measure(spec.name).x <= name_right) {
        detail::draw_highlighted(DrawList::window(), {x, text_y}, spec.name, spec.dimmed ? std::string_view{} : spec.query,
                                 ink);
    } else {
        // Too long for the row: cut with an ellipsis, whole in the tooltip.
        draw_text({x, text_y}, ink, ellipsized(spec.name, name_right - x));
        // Not during a drag, whose preview would be replaced.
        if (hovered && !context.dragging()) item_tooltip(spec.name);
    }

    // Nothing but the rename field is an item after the row's own, so the
    // row stays the item just drawn for the owner's menus and drags. The
    // field moved the cursor; the next row starts under this one.
    if (renaming) {
        set_cursor({min.x, max.y});
        k.item = {.id = row_id, .rect = row, .takes = true, .in = in};
    }
    k.row_drawn = 0;
    result.open = spec.has_children && (open || spec.force_open);
    return result;
}

bool tree_row_open(const char* id, bool starts_open) {
    IdScope scope(id);
    return stored_bool(ui().id("##open"), starts_open);
}

void open_tree_row(const char* id) {
    IdScope scope(id);
    store_bool(ui().id("##open"), true);
}

RowsInView::RowsInView(std::size_t count, std::optional<std::size_t> revealed)
    : RowsInView(count, RowPitch{.pitch = row_height()}) {
    if (revealed) reveal(*revealed);
}

DropPlace drop_place() {
    const float y = mouse_pos().y;
    const float top = item_min().y;
    const float height = item_max().y - top;
    if (y < top + height * 0.25f) return DropPlace::Before;
    if (y > top + height * 0.75f) return DropPlace::After;
    return DropPlace::Into;
}

void draw_drop(DropPlace place, int depth) {
    const glm::vec2 min = item_min();
    const glm::vec2 max = item_max();
    core::DrawList& dl = ui().draw_list();
    const Color accent = faded(theme::accent());
    if (place == DropPlace::Into) {
        dl.rect_filled(min, max, faded(theme::drop_fits()), ROUNDING);
        dl.rect(min, max, accent, ROUNDING);
        return;
    }
    // The line starts where a row at `depth` has its icon, so the ring's
    // indent says which parent the rows land under.
    const float y = std::floor(place == DropPlace::Before ? min.y : max.y);
    const float x = min.x + ROW_PAD + static_cast<float>(depth) * indent() + twisty() + PART_GAP;
    dl.line({x, y}, {max.x - ROW_PAD, y}, accent, INSERT_LINE);
    dl.circle_filled({x, y}, RING * 0.5f, faded(theme::surface_base()));
    dl.circle({x, y}, RING * 0.5f - 1.0f, accent, INSERT_LINE);
}

void drag_preview(const char* icon, std::optional<theme::Hue> hue, std::string_view text,
                  std::string_view refusal) {
    if (icon != nullptr) {
        ui::text(icon, hue ? theme::hue(*hue) : theme::text_secondary());
        same_line(PART_GAP);
    }
    ui::text(text);
    if (!refusal.empty()) {
        const std::string why(refusal);
        status(Severity::Error, why.c_str());
    }
}

} // namespace ftk::ui
