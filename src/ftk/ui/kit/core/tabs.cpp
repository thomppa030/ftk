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
#include <cstddef>
#include <cstdint>
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
// How far above or below its row, in rows, a document tab is off it, as a
// docked window's tab is.
constexpr float OFF_ROW = 1.5f;
// The line under a row of tabs, in the colour of the one in front.
constexpr float BAR_BORDER = 1.0f;
// How many frames a row waits, after a tab is let go on it, for the program
// to draw that tab there.
constexpr uint64_t LANDING_FRAMES = 3;
// A tab that docks, carried back over its row and held on another tab this
// long, brings that one forward.
constexpr double HOLD_SECONDS = 0.6;
// The dot an unsaved document's tab shows, as ImGui's.
constexpr Color UNSAVED{1.0f, 1.0f, 1.0f, 1.0f};

// How tabs are drawn when nothing says otherwise: as the ImGui kit's theme
// coloured them.
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

// `at` in the OS window `from` as the window `to` has it: the same point in
// the same window, wherever it is; in another, where the platform says
// where both are, and none where it doesn't.
std::optional<glm::vec2> point_in_root(core::RootId from, glm::vec2 at, core::RootId to) {
    if (from == to) return at;
    return ui().point_in(from, at, to);
}

// Where in `bar` a tab let go at `at` lands, between the tabs whose middles
// it is between; none unless `at` is over the row, a row's height above or
// below it at most.
std::optional<std::size_t> place_in_row(const TabBar& bar, glm::vec2 at) {
    const float slack = bar.rect.height();
    if (at.x < bar.rect.min.x || at.x >= bar.rect.max.x || at.y < bar.rect.min.y - slack ||
        at.y >= bar.rect.max.y + slack) {
        return std::nullopt;
    }
    const float spacing = kit().style.item_inner_spacing.x;
    std::size_t index = 0;
    float x = bar.rect.min.x;
    for (const TabBar::Tab& tab : bar.tabs) {
        if (at.x < x + tab.width * 0.5f) break;
        x += tab.width + spacing;
        ++index;
    }
    return index;
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
    if (face.outline) {
        dl.rect({rect.min.x, top}, {rect.max.x, bottom}, faded(theme::surface_active()), rounding, RoundCorners::Top, 1.0f);
        context.semantics(id, core::Role::Tab, face.label, {}, {.selected = face.front}, rect);
        return drawn;
    }
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
    glm::vec2 text_at = rect.min + style.frame_padding;
    if (face.icon != nullptr) {
        draw_text(text_at, faded(face.icon_ink), face.icon);
        text_at.x += measure(face.icon).x + style.item_inner_spacing.x;
    }
    draw_text(text_at, face.ink, face.label);
    dl.pop_clip();
    if (dot_shown) {
        dl.circle_filled(button_at + glm::vec2{button * 0.5f}, button * 0.2f, faded(UNSAVED));
    } else if (close_shown) {
        draw_close(close, close_in);
        drawn.closed = close_in.clicked;
    }
    // A middle click on it closes it too.
    if (face.closable && drawn.hovered && mouse_clicked(MouseButton::Middle)) drawn.closed = true;
    // At its own rectangle: a tab is placed by hand, not by the layout.
    context.semantics(id, core::Role::Tab, face.label, {}, {.selected = face.front}, rect);
    return drawn;
}

void draw_tab_copy(std::string_view label, const char* icon, Color icon_ink, bool own_window) {
    const Kit& k = kit();
    core::Context& context = ui();
    context.begin_drag_preview();
    const core::Rect shown = context.panel_rect();
    context.draw_list().rect_filled(shown.min, shown.max, faded(theme::surface_base()), look().rounding);
    if (icon != nullptr) {
        text(icon, icon_ink);
        same_line(k.style.item_inner_spacing.x);
    }
    text(label);
    if (own_window) {
        same_line();
        text("opens in its own window", theme::text_secondary());
    }
    context.end_drag_preview();
}

bool land_panel(Id window, glm::vec2 at) {
    Kit& k = kit();
    const core::Context& context = ui();
    // Rows of every window the platform says where it is, this one's too.
    for (auto& [id, bar] : k.tab_bars) {
        if (!bar.takes_panels || bar.frame + 1 < context.frame()) continue;
        const std::optional<glm::vec2> here = point_in_root(context.root(), at, bar.root);
        const std::optional<std::size_t> place = here ? place_in_row(bar, *here) : std::nullopt;
        if (!place) continue;
        bar.panel_landed = std::pair{window, *place};
        bar.landing = *place;
        bar.landing_until = context.frame() + LANDING_FRAMES;
        return true;
    }
    return false;
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

DocumentTabs::DocumentTabs(const char* id, const DocumentTabsOptions& options) {
    core::Context& context = ui();
    Kit& k = kit();
    const Id bar_id = context.id(id);
    TabBar& bar = k.tab_bars[bar_id];
    bar.reorder = true;
    bar.group = options.group != nullptr ? core::hash_label(options.group, 0) : 0;
    bar.owner = options.owner;
    bar.takes_panels = options.takes_panels;
    bar.frame = context.frame();
    // A panel let go on it since it was drawn, for the program to show as a
    // tab.
    if (bar.panel_landed) {
        if (const auto window = k.windows.find(bar.panel_landed->first); window != k.windows.end()) {
            landed_ = LandedPanel{.window = window->second.name, .place = bar.panel_landed->second};
        }
        bar.panel_landed.reset();
    }
    bar.root = context.root();
    bar.rect = context.layout().next({std::max(space_left().x, 1.0f), frame_height()});
    for (TabBar::Tab& tab : bar.tabs) tab.seen = false;

    // A tab of another row of its group carried over this one, a row's
    // height above or below it at most: the row parts where it would land,
    // between the tabs whose middles it is between. Last frame's carry
    // counts, for a row drawn before the one it was carried from.
    bar.gap.reset();
    CarriedTab& carried = k.carried_tab;
    if (bar.group != 0 && carried.group == bar.group && carried.bar != bar_id && carried.frame + 1 >= context.frame()) {
        const std::optional<glm::vec2> here = context.point_in(carried.root, carried.at, bar.root);
        if (const std::optional<std::size_t> place = here ? place_in_row(bar, *here) : std::nullopt) {
            bar.gap = *place;
            bar.gap_width = carried.width;
            carried.over = bar_id;
            carried.over_owner = bar.owner;
            carried.over_index = *place;
            carried.over_frame = context.frame();
        } else if (carried.over == bar_id) {
            carried.over = 0;
        }
    }
    // A panel that lands on rows, carried by its tab over it, parts it too,
    // from this window or another where the platform says where they are.
    // Last frame's carry counts, for a row drawn before the panel.
    if (!bar.gap && bar.takes_panels && k.carried.window != 0 && k.carried.lands_on_rows &&
        k.carried.frame + 1 >= context.frame() && mouse_down(MouseButton::Left)) {
        const auto window = k.windows.find(k.carried.window);
        const std::optional<glm::vec2> here = point_in_root(k.carried.root, k.carried.at, bar.root);
        if (const std::optional<std::size_t> place = here ? place_in_row(bar, *here) : std::nullopt;
            place && window != k.windows.end()) {
            bar.gap = *place;
            bar.gap_width = tab_width(shown(window->second.name), false);
            k.carried.over_row_frame = context.frame();
        }
    }
    // Its own tab that docks, carried off it and back over it: the tab held
    // under it, timed from when it came under it.
    Id held = 0;
    if (carried.docks && carried.bar == bar_id && carried.root == bar.root && carried.frame + 1 >= context.frame()) {
        const glm::vec2 at = carried.at;
        const float slack = bar.rect.height();
        if (at.x >= bar.rect.min.x && at.x < bar.rect.max.x && at.y >= bar.rect.min.y - slack &&
            at.y < bar.rect.max.y + slack) {
            const float spacing = k.style.item_inner_spacing.x;
            float x = bar.rect.min.x;
            for (const TabBar::Tab& tab : bar.tabs) {
                if (tab.id != carried.tab && at.x >= x && at.x < x + tab.width) held = tab.id;
                x += tab.width + spacing;
            }
        }
    }
    if (held != bar.hold) {
        bar.hold = held;
        bar.hold_since = seconds();
    }
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
    // Where a tab carried over it would land: an empty tab's place, washed
    // and outlined in the accent.
    if (bar.gap) {
        const float spacing = k.style.item_inner_spacing.x;
        float x = bar.rect.min.x;
        for (std::size_t i = 0; i < std::min(*bar.gap, bar.tabs.size()); ++i) x += bar.tabs[i].width + spacing;
        const float rounding = std::max(0.0f, std::min(look().rounding, bar.gap_width * 0.5f - 1.0f));
        const glm::vec2 min{x, bar.rect.min.y + 1.0f};
        const glm::vec2 max{x + bar.gap_width, bar.rect.max.y - BAR_BORDER};
        core::DrawList& dl = ui().draw_list();
        dl.rect_filled(min, max, faded(theme::selection()), rounding, RoundCorners::Top);
        dl.rect(min, max, faded(theme::accent()), rounding, RoundCorners::Top, 1.0f);
    }
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
    // A new one at the end, or where a tab was let go on the row.
    auto at = std::ranges::find(bar.tabs, id, &TabBar::Tab::id);
    if (at == bar.tabs.end()) {
        std::size_t place = bar.tabs.size();
        if (bar.landing && context.frame() <= bar.landing_until) place = std::min(*bar.landing, place);
        bar.landing.reset();
        at = bar.tabs.insert(bar.tabs.begin() + static_cast<std::ptrdiff_t>(place), TabBar::Tab{.id = id});
    }
    const std::string_view label = shown(tab.label);
    at->width = tab_width(label, tab.closable || tab.unsaved);
    if (tab.icon != nullptr) at->width += measure(tab.icon).x + k.style.item_inner_spacing.x;
    at->seen = true;
    float x = bar.rect.min.x;
    for (auto before = bar.tabs.begin(); before != at; ++before) x += before->width + spacing;
    // Past where a tab carried over the row would land.
    if (bar.gap && static_cast<std::size_t>(at - bar.tabs.begin()) >= *bar.gap) x += bar.gap_width + spacing;

    // Asked forward, or the first drawn while none is in front.
    if (tab.select || bar.front == 0) bar.front = id;
    const core::Rect rect{.min = {x, bar.rect.min.y}, .max = {x + at->width, bar.rect.max.y}};
    const DrawnTab drawn = draw_tab(id, rect,
                                    {.label = label,
                                     .ink = corekit::text_colour(),
                                     .icon = tab.icon,
                                     .icon_ink = tab.hue ? theme::hue(*tab.hue) : theme::text_secondary(),
                                     .front = bar.front == id,
                                     .closable = tab.closable,
                                     .unsaved = tab.unsaved,
                                     .focused = window_focused(),
                                     .outline = at->off_row});
    // A press brings it forward.
    if (drawn.in.pressed) bar.front = id;
    // Held under its row's tab that docks, carried back over the row: a line
    // grows along its foot, and held long enough it comes forward.
    if (bar.hold == id) {
        const auto held_for = static_cast<float>((seconds() - bar.hold_since) / HOLD_SECONDS);
        const float done = std::clamp(held_for, 0.0f, 1.0f);
        const float foot = rect.max.y - BAR_BORDER;
        context.draw_list().rect_filled({rect.min.x, foot - 2.0f}, {rect.min.x + rect.width() * done, foot},
                                        faded(theme::toggle_on()));
        if (done >= 1.0f) {
            bar.front = id;
        } else {
            context.request_frame();
        }
    }

    // Dragged a row and a half above or below the row, or past the
    // window's side, it is off it and keeps its place; let go past the
    // window's edge, it is torn off.
    if (drawn.in.held && drawn.in.dragging) {
        const glm::vec2 p = drawn.in.position;
        const float rows = bar.rect.height() * OFF_ROW;
        const bool away = p.y < bar.rect.min.y - rows || p.y > bar.rect.max.y + rows || p.x < 0.0f ||
                          p.x >= context.root_size().x;
        // One that docks stays off it once lifted, until let go.
        at->off_row = (tab.tear_off || tab.docks) && (away || (tab.docks && at->off_row));
    }
    // Off a row of a group, it is carried: the group's other rows look for
    // it, and part where it would land. One that docks is carried too, for
    // its own row to bring forward the tab it is held over.
    CarriedTab& carried = k.carried_tab;
    if (at->off_row && drawn.in.held && (bar.group != 0 || tab.docks)) {
        if (carried.bar != k.tab_bar || carried.tab != id) carried = {.bar = k.tab_bar, .tab = id};
        carried.group = bar.group;
        carried.docks = tab.docks;
        carried.share = tab.dock_share;
        carried.home = tab.dockspace;
        carried.root = context.root();
        carried.at = drawn.in.position;
        carried.width = at->width;
        carried.frame = context.frame();
    }
    const bool over_a_row = carried.bar == k.tab_bar && carried.tab == id && carried.over != 0 &&
                            carried.over_frame + 1 >= context.frame();
    // One that docks, carried in its window over no row: the targets of the
    // dockspace under it, and where it would dock let go.
    if (tab.docks && at->off_row && drawn.in.held && carried.bar == k.tab_bar && carried.tab == id) {
        result.lifted = true;
        const glm::vec2 p = drawn.in.position;
        const glm::vec2 room = context.root_size();
        const bool inside = p.x >= 0.0f && p.y >= 0.0f && p.x < room.x && p.y < room.y;
        carried.place = inside && !over_a_row ? carry_tab_over_docks(id, p, tab.dock_share, tab.dockspace) : std::nullopt;
    }
    // Off the row, a copy of it follows the pointer, in a window of its own
    // past the window's edge, and says there what letting go does.
    if (at->off_row && drawn.in.held) {
        const glm::vec2 p = drawn.in.position;
        const glm::vec2 room = context.root_size();
        const bool docking_elsewhere = carried.bar == k.tab_bar && carried.tab == id && carried.elsewhere &&
                                       carried.elsewhere_frame + 1 >= context.frame();
        const bool outside =
            (p.x < 0.0f || p.y < 0.0f || p.x >= room.x || p.y >= room.y) && !over_a_row && !docking_elsewhere;
        draw_tab_copy(label, tab.icon, tab.hue ? theme::hue(*tab.hue) : theme::text_secondary(),
                      outside && tab.tear_off);
    }
    // Dragged past the middle of the tab beside it, it takes that one's place.
    if (bar.reorder && drawn.in.held && drawn.in.dragging && !at->off_row) {
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
    // Let go, after its last move: on another row of its group, it lands
    // there, where the row parted; past the window's edge, it is torn off.
    if (drawn.in.released) {
        const glm::vec2 let_go = drawn.in.position;
        const glm::vec2 room = context.root_size();
        const bool outside = let_go.x < 0.0f || let_go.y < 0.0f || let_go.x >= room.x || let_go.y >= room.y;
        // In its own window, or over another window's dock target lately.
        const bool ours = tab.docks && carried.bar == k.tab_bar && carried.tab == id;
        const bool elsewhere = ours && carried.elsewhere && carried.elsewhere_frame + 1 >= context.frame();
        const bool docking = ours && (carried.place || elsewhere);
        if (at->off_row && over_a_row) {
            result.landed_on = carried.over_owner;
            if (const auto onto = k.tab_bars.find(carried.over); onto != k.tab_bars.end()) {
                onto->second.landing = carried.over_index;
                onto->second.landing_until = context.frame() + LANDING_FRAMES;
            }
        } else if (at->off_row && docking) {
            result.docked = carried.place ? carried.place : carried.elsewhere;
        } else if (tab.tear_off && outside) {
            result.torn_off = let_go;
        }
        if (carried.bar == k.tab_bar && carried.tab == id) carried = {};
        at->off_row = false;
    }
    result.front = bar.front == id;
    result.place = static_cast<std::size_t>(std::ranges::find(bar.tabs, id, &TabBar::Tab::id) - bar.tabs.begin());
    result.closed = tab.closable && drawn.closed;
    return result;
}

} // namespace ftk::ui
