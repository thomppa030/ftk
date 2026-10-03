#include "ftk/ui/kit/tabs.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/state.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/kit/window.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float STRIP_PAD = 6.0f;  // above the tabs and before the first
// The tabs name what the panel shows, so they read a step over its text.
constexpr float TAB_TEXT = 16.0f;
constexpr glm::vec2 TAB_PADDING{14.0f, 8.0f};
constexpr Color CLEAR{0.0f, 0.0f, 0.0f, 0.0f};
// The line under a row of tabs, in the colour of the one in front.
constexpr float BAR_BORDER = 1.0f;
// The dot an unsaved document's tab shows, as ImGui's.
constexpr Color UNSAVED{1.0f, 1.0f, 1.0f, 1.0f};

// How tabs are drawn when nothing says otherwise: as the ImGui kit's theme
// colours them.
TabLook theme_look() {
    return {.rest = theme::surface_sunken(),
            .hover = theme::surface_raised(),
            .front = theme::surface_base(),
            .rest_dimmed = theme::surface_sunken(),
            .front_dimmed = theme::surface_between()};
}

TabLook look() {
    const Kit& k = kit();
    return k.tab_looks.empty() ? theme_look() : k.tab_looks.back();
}

} // namespace

namespace corekit {

float tab_width(std::string_view label, bool marked) {
    const Style& style = kit().style;
    const float width = measure(label).x + style.frame_padding.x;
    if (marked) return width + style.frame_padding.x + style.item_inner_spacing.x + text_style().size;
    return width + style.frame_padding.x + 1.0f;
}

void draw_bar_line(core::Rect rect, bool focused) {
    const TabLook l = look();
    ui().draw_list().rect_filled({rect.min.x, rect.max.y - BAR_BORDER}, rect.max,
                                 faded(focused ? l.front : l.front_dimmed));
}

// Its × placed first, over it, then its shape, its label and the × or the
// unsaved dot.
DrawnTab draw_tab(Id id, core::Rect rect, const TabFace& face) {
    core::Context& context = ui();
    const Style& style = kit().style;
    const TabLook l = look();
    DrawnTab drawn;

    // Whether it was under the mouse last frame, to show its × now.
    const Id hover_id = core::hash_label("#HOVER", id);
    const float button = text_style().size;
    const glm::vec2 button_at{std::max(rect.min.x, rect.max.x - style.frame_padding.x - button),
                              rect.min.y + style.frame_padding.y};
    const bool close_shown = face.closable && (face.front || stored_bool(hover_id));
    const core::Rect close = close_rect(button_at);
    const core::Interaction close_in =
        close_shown ? context.interact(core::hash_label("#CLOSE", id), close, {.beneath = true}) : core::Interaction{};
    drawn.in = place(id, rect);
    drawn.hovered = drawn.in.hovered || close_in.hovered;
    store_bool(hover_id, drawn.hovered || drawn.in.held || close_in.held);

    core::DrawList& dl = context.draw_list();
    const float rounding = std::max(0.0f, std::min(l.rounding, rect.width() * 0.5f - 1.0f));
    const float top = rect.min.y + 1.0f;
    const float bottom = rect.max.y - BAR_BORDER;
    const Color fill = drawn.in.held || drawn.hovered ? l.hover
                       : face.front                   ? (face.focused ? l.front : l.front_dimmed)
                                                      : (face.focused ? l.rest : l.rest_dimmed);
    if (fill.a > 0.0f) dl.rect_filled({rect.min.x, top}, {rect.max.x, bottom}, faded(fill), rounding, RoundCorners::Top);
    if (l.border > 0.0f) {
        // Its sides and top: the bottom is open to what it shows.
        dl.push_clip({rect.min.x, top}, {rect.max.x, bottom - l.border});
        dl.rect({rect.min.x, top}, {rect.max.x, bottom}, faded(theme::border()), rounding, RoundCorners::Top, l.border);
        dl.pop_clip();
    }
    if (face.front && l.overline) {
        dl.rect_filled({rect.min.x + rounding * 0.5f, top}, {rect.max.x - rounding * 0.5f, top + 1.0f},
                       faded(*l.overline));
    }

    // The unsaved dot gives way to the × under the mouse.
    const bool dot_shown = face.unsaved && button_at.x + button <= rect.max.x && (!close_shown || !drawn.hovered);
    float text_right = rect.max.x - style.frame_padding.x;
    if (dot_shown || close_shown) text_right -= button * (dot_shown || face.front ? 0.9f : 1.0f);
    dl.push_clip(rect.min, {text_right, rect.max.y});
    draw_text(rect.min + style.frame_padding, face.ink, face.label);
    dl.pop_clip();
    if (dot_shown) {
        dl.circle_filled(button_at + glm::vec2{button * 0.5f}, button * 0.2f, faded(UNSAVED));
    } else if (close_shown) {
        draw_close(close, close_in);
        drawn.closed = close_in.clicked;
    }
    // A middle click on it closes it too.
    if (face.closable && drawn.hovered && mouse_clicked(MouseButton::Middle)) drawn.closed = true;
    context.semantics(id, core::Role::Tab, face.label, {}, {.selected = face.front});
    return drawn;
}

} // namespace corekit

TabStripResult tab_strip(const char* id, const TabStripSpec& spec) {
    TabStripResult result;
    core::Context& context = ui();
    IdScope scope(id);

    // The tabs sit in a sunken strip across the panel; the one in front is
    // the panel's own colour, joined to what it shows below, and the rest
    // are its text alone until hovered.
    const glm::vec2 start = cursor();
    const float width = space_left().x;
    {
        FontScope font(Font::Current, TAB_TEXT);
        Style style = kit().style;
        style.frame_padding = TAB_PADDING;
        push_style(style);
        kit().tab_looks.push_back({.rest = CLEAR,
                                   .hover = theme::surface_hover(),
                                   .front = theme::surface_base(),
                                   .rest_dimmed = CLEAR,
                                   .front_dimmed = theme::surface_base(),
                                   .overline = theme::hue(spec.hue)});

        const float tab_h = frame_height();
        const float strip_h = STRIP_PAD + tab_h;
        core::DrawList& dl = context.draw_list();
        dl.rect_filled(start, {start.x + width, start.y + strip_h}, faded(theme::surface_sunken()));
        const core::Rect bar{.min = {start.x + STRIP_PAD, start.y + STRIP_PAD}, .max = {start.x + width, start.y + strip_h}};
        draw_bar_line(bar, true);
        const float spacing = kit().style.item_inner_spacing.x;

        // The tab that adds one keeps to the right end; the others run from
        // the left, cut off short of it.
        const std::string add = std::string(icon::add) + "  " + spec.add_label;
        const float add_w = tab_width(add, false);
        const core::Rect add_rect{.min = {bar.max.x - add_w, bar.min.y}, .max = bar.max};
        const bool can_remove = spec.names.size() > 1;
        dl.push_clip(bar.min, {add_rect.min.x - spacing, bar.max.y});
        float x = bar.min.x;
        for (int i = 0; i < static_cast<int>(spec.names.size()); ++i) {
            IdScope tab_scope(i);
            // The tab's id is its place, so a rename keeps it.
            const std::string& name = spec.names[static_cast<std::size_t>(i)];
            const float tab_w = tab_width(name, can_remove);
            const bool front = i == spec.selected;
            const DrawnTab drawn = draw_tab(context.id("##tab"), {.min = {x, bar.min.y}, .max = {x + tab_w, bar.max.y}},
                                            {.label = name,
                                             .ink = front ? theme::text() : theme::text_secondary(),
                                             .front = front,
                                             .closable = can_remove});
            if (drawn.in.hovered && drawn.in.double_clicked) result.rename = i;
            if (can_remove && drawn.hovered) tooltip(spec.remove_label);
            // Reported only when the user brings it forward, not when the
            // caller moves the selection.
            if (drawn.in.pressed && !front) result.selected = i;
            if (drawn.closed) result.removed = i;
            x += tab_w + spacing;
        }
        dl.pop_clip();
        const DrawnTab added =
            draw_tab(context.id("##add"), add_rect, {.label = add, .ink = theme::text_secondary()});
        if (added.in.clicked) result.add = true;

        kit().tab_looks.pop_back();
        pop_style();
        // What the tabs show starts under the strip.
        set_cursor({start.x, start.y + strip_h + theme::GAP_S});
    }
    reserve({width, 0.0f});
    return result;
}

ContextTabBar::ContextTabBar(const char* id) {
    // The tab padding first, so the bar is as tall as a tab with it.
    Style style = kit().style;
    style.frame_padding = TAB_PADDING;
    push_style(style);
    const core::Rect rect = take_edge_bar(true, frame_height() + 4.0f);
    next_window_place(rect.min, rect.size());
    open_ = begin_window(id,
                         {.title_bar = false,
                          .resize = false,
                          .move = false,
                          .collapse = false,
                          .scrollbar = false,
                          .background = false,
                          .docking = false,
                          .nav_focus = false,
                          .remembered = false},
                         {4.0f, 2.0f});
    const core::Rect shown = ui().area_rect();
    ui().draw_list().rect_filled(shown.min, shown.max, faded(theme::surface_sunken()));

    Color resting = theme::surface_sunken();
    resting.a = 0.0f;
    TabLook tabs = theme_look();
    tabs.rest = resting;
    tabs.hover = theme::surface_raised();
    tabs.front = theme::surface_base();
    tabs.rounding = 4.0f;
    tabs.border = 1.0f;
    kit().tab_looks.push_back(tabs);
}

ContextTabBar::~ContextTabBar() {
    kit().tab_looks.pop_back();
    end_window();
    pop_style();
}

DocumentTabs::DocumentTabs(const char* id) {
    core::Context& context = ui();
    Kit& k = kit();
    const Id bar_id = context.id(id);
    TabBar& bar = k.tab_bars[bar_id];
    bar.reorder = true;
    bar.rect = context.layout().next({std::max(space_left().x, 1.0f), frame_height()});
    for (TabBar::Tab& tab : bar.tabs) tab.seen = false;
    draw_bar_line(bar.rect, window_focused());
    // Its tabs' ids under its own.
    context.ids().push(bar_id);
    k.tab_bar = bar_id;
    open_ = true;
}

DocumentTabs::~DocumentTabs() {
    if (!open_) return;
    Kit& k = kit();
    TabBar& bar = k.tab_bars[k.tab_bar];
    // The tabs not drawn this frame are gone, the one in front with them.
    std::erase_if(bar.tabs, [](const TabBar::Tab& tab) { return !tab.seen; });
    if (std::ranges::none_of(bar.tabs, [&](const TabBar::Tab& tab) { return tab.id == bar.front; })) bar.front = 0;
    ui().ids().pop();
    k.tab_bar = 0;
}

DocumentTabResult document_tab(const DocumentTab& tab) {
    DocumentTabResult result;
    Kit& k = kit();
    const auto found = k.tab_bars.find(k.tab_bar);
    if (found == k.tab_bars.end()) return result;
    TabBar& bar = found->second;
    core::Context& context = ui();
    const Id id = context.id(tab.label);
    const float spacing = k.style.item_inner_spacing.x;

    // Its place in the bar's order, a new tab at the end, and as wide as
    // its label now.
    auto at = std::ranges::find(bar.tabs, id, &TabBar::Tab::id);
    if (at == bar.tabs.end()) {
        bar.tabs.push_back({.id = id});
        at = std::prev(bar.tabs.end());
    }
    at->width = tab_width(tab.label, tab.closable || tab.unsaved);
    at->seen = true;
    float x = bar.rect.min.x;
    for (auto before = bar.tabs.begin(); before != at; ++before) x += before->width + spacing;

    // Asked forward, or the first drawn while none is in front.
    if (tab.select || bar.front == 0) bar.front = id;
    const core::Rect rect{.min = {x, bar.rect.min.y}, .max = {x + at->width, bar.rect.max.y}};
    const DrawnTab drawn = draw_tab(id, rect,
                                    {.label = tab.label,
                                     .ink = corekit::text_colour(),
                                     .front = bar.front == id,
                                     .closable = tab.closable,
                                     .unsaved = tab.unsaved,
                                     .focused = window_focused()});
    // A press brings it forward.
    if (drawn.in.pressed) bar.front = id;

    // Dragged past the middle of the tab beside it, it takes that one's place.
    if (bar.reorder && drawn.in.held && drawn.in.dragging) {
        const float pointer = drawn.in.position.x;
        if (at != bar.tabs.begin()) {
            const auto left = std::prev(at);
            if (pointer < x - spacing - left->width * 0.5f) {
                std::iter_swap(left, at);
                context.request_frame();
            }
        }
        const auto right = std::next(at);
        if (right != bar.tabs.end() && pointer > x + at->width + spacing + right->width * 0.5f) {
            std::iter_swap(right, at);
            context.request_frame();
        }
    }
    result.front = bar.front == id;
    result.closed = tab.closable && drawn.closed;
    return result;
}

} // namespace ftk::ui
