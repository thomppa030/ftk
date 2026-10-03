#include "ftk/ui/kit/pages.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"

#include <algorithm>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float ENTRY_HEIGHT = 26.0f;
constexpr float TITLE_SIZE = 16.0f;

} // namespace

PagedWindow::PagedWindow(const char* title, bool* open, glm::vec2 size) {
    // First shown centred at `size`, then where it was left.
    kit().next_window.pos_once = (ui().root_size() - size) * 0.5f;
    next_window_size_once(size);
    // The list and the page run to the window's edges; each pads itself.
    open_ = begin_window(title, {.collapse = false, .scrollbar = false, .docking = false}, {0.0f, 0.0f}, open);
}

PagedWindow::~PagedWindow() {
    end_window();
}

PageBody::PageBody(const char* id, float reserve_bottom) {
    core::Context& context = ui();
    same_line(0.0f);
    const float height = std::max(space_left().y - reserve_bottom, 1.0f);
    context.begin_region(id, {core::FILL, height}, {.padding = {theme::GAP_L, theme::GAP_M + theme::GAP_XS}});
    open_ = !context.area_rect().empty();
}

PageBody::~PageBody() {
    core::Context& context = ui();
    context.end_region();
    place_still(context.layout().last_item());
}

PageList::PageList(const char* id, float reserve_bottom) {
    core::Context& context = ui();
    Style style = kit().style;
    style.item_spacing = {theme::GAP_S, theme::GAP_XS};
    push_style(style);
    const float height = std::max(space_left().y - reserve_bottom, 1.0f);
    context.begin_region(id, {PAGE_LIST_WIDTH, height}, {.padding = {theme::GAP_M, theme::GAP_M}});
    const core::Rect rect = context.area_rect();
    context.draw_list().rect_filled(rect.min, rect.max, faded(theme::surface_sunken()));
    open_ = !rect.empty();
}

PageList::~PageList() {
    core::Context& context = ui();
    context.end_region();
    place_still(context.layout().last_item());
    pop_style();
}

bool page_entry(const char* icon, const char* label, bool selected, bool dimmed) {
    core::Context& context = ui();
    const core::Rect rect = context.layout().next({std::max(space_left().x, 1.0f), ENTRY_HEIGHT});
    Id id = 0;
    {
        IdScope scope(label);
        id = context.id("##page");
    }
    const core::Interaction in = place(id, rect);
    context.semantics(id, core::Role::ListItem, label, {}, {.selected = selected});

    core::DrawList& dl = context.draw_list();
    const float rounding = kit().style.frame_rounding;
    if (selected) {
        dl.rect_filled(rect.min, rect.max, faded(theme::selection()), rounding);
    } else if (in.hovered) {
        dl.rect_filled(rect.min, rect.max, faded(theme::surface_hover()), rounding);
    }

    const Color ink = dimmed                      ? theme::text_disabled()
                      : selected || in.hovered    ? theme::text()
                                                  : theme::text_secondary();
    const Color icon_ink = dimmed ? theme::text_disabled() : selected ? theme::accent() : ink;
    const float y = rect.min.y + (ENTRY_HEIGHT - line_height()) * 0.5f;
    const float x = rect.min.x + theme::GAP_M;
    draw_text({x, y}, icon_ink, icon);
    draw_text({x + measure(icon).x + theme::GAP_M, y}, ink, label);
    return in.clicked;
}

void page_title(const char* icon, const char* label) {
    {
        FontScope bold(Font::Bold, TITLE_SIZE);
        text(icon, theme::accent());
        same_line(theme::GAP_M);
        text(label);
    }
    reserve({0.0f, theme::GAP_S});
}

} // namespace ftk::ui
