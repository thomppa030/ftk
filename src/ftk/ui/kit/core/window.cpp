#include "ftk/ui/kit/window.hpp"

#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"

#include <algorithm>
#include <utility>

namespace ftk::ui {

namespace corekit {

namespace {

// Where a window opens when nothing places it.
constexpr glm::vec2 FIRST_PLACE{60.0f, 60.0f};

Id window_id(const char* name) {
    return core::hash_label(name, 0);
}

bool any_button_pressed() {
    return mouse_clicked(MouseButton::Left) || mouse_clicked(MouseButton::Right) || mouse_clicked(MouseButton::Middle);
}

// The window's scrollbar down its right edge, while what it holds is
// taller than it: its grab dragged scrolls it.
void scrollbar(Window& window, Id id) {
    core::Context& context = ui();
    const float room = context.scroll_max().y;
    if (room <= 0.0f) return;
    const Style& style = kit().style;
    const core::Rect& rect = window.rect;
    const core::Rect track{.min = {rect.max.x - style.scrollbar_size, rect.min.y + window.bar}, .max = rect.max};
    const float visible = track.height();
    const float grab = std::max(visible * visible / (visible + room), style.scrollbar_size * 2.0f);
    const float travel = std::max(track.height() - grab, 1.0f);
    const float offset = context.scroll().y;
    const core::Rect thumb{.min = {track.min.x, track.min.y + travel * offset / room},
                           .max = {track.max.x, track.min.y + travel * offset / room + grab}};
    const core::Interaction in = context.interact(core::hash_label("#SCROLLY", id), thumb, {.beneath = true});
    if (in.pressed) window.grab_from = offset;
    if (in.held) {
        const float moved = (in.position.y - in.press_position.y) / travel * room;
        context.set_scroll({context.scroll().x, std::clamp(window.grab_from + moved, 0.0f, room)});
    }
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(track.min, track.max, faded(theme::surface_sunken() * glm::vec4{1.0f, 1.0f, 1.0f, 0.5f}));
    const Color colour = in.held ? theme::surface_active() : (in.hovered ? theme::surface_hover() : theme::surface_raised());
    dl.rect_filled(thumb.min + glm::vec2{2.0f, 2.0f}, thumb.max - glm::vec2{2.0f, 2.0f}, faded(colour),
                   style.scrollbar_size * 0.5f);
}

} // namespace

bool begin_window(const char* name, const WindowOptions& options, glm::vec2 padding) {
    core::Context& context = ui();
    Kit& k = kit();
    keep_item();
    const Id id = window_id(name);
    Window& window = k.windows[id];
    const bool again = window.frame == context.frame();
    if (again) {
        // Drawn again over what it holds.
        context.begin_panel(id, window.rect, {.padding = window.padding, .bar = window.bar});
        context.draw_list().push_clip(window.rect.min + glm::vec2{0.0f, window.bar}, window.rect.max);
        k.window_stack.push_back(id);
        return true;
    }

    // Shown now, not last frame: it comes forward and takes the keyboard,
    // as a window does when it appears.
    const bool appearing = window.frame == 0 || window.frame + 1 < context.frame();
    if (window.frame == 0) {
        window.name = name;
        window.rect = {.min = FIRST_PLACE, .max = FIRST_PLACE};
    }
    const NextWindow next = std::exchange(k.next_window, {});
    glm::vec2 size = window.rect.size();
    if (next.size_once && !window.sized) {
        size = *next.size_once;
        window.sized = true;
    }
    if (next.size) {
        size = *next.size;
        window.sized = true;
    }
    if (next.size_limits) size = glm::clamp(size, next.size_limits->first, next.size_limits->second);
    if (next.pos) window.rect.min = *next.pos;
    window.rect.max = window.rect.min + size;
    window.frame = context.frame();
    window.options = options;
    window.padding = padding;
    window.bar = options.title_bar ? frame_height() : 0.0f;

    context.begin_panel(id, window.rect,
                        {.padding = padding, .bar = window.bar, .fit = !window.sized, .raise = options.to_front});
    // A window that fits what it holds is as big as that came to.
    window.rect = context.panel_rect();
    const core::Rect& rect = window.rect;

    if ((appearing && options.nav_focus) || (any_button_pressed() && context.area_or_inner_under_pointer())) {
        k.focused_window = id;
    }

    core::DrawList& dl = context.draw_list();
    if (options.background) dl.rect_filled(rect.min, rect.max, faded(theme::surface_base()));
    if (window.bar > 0.0f) {
        const core::Rect bar{.min = rect.min, .max = {rect.max.x, rect.min.y + window.bar}};
        dl.rect_filled(bar.min, bar.max, faded(theme::surface_sunken()));
        dl.push_clip(bar.min, bar.max);
        draw_text(bar.min + k.style.frame_padding, theme::text(), shown(name));
        dl.pop_clip();
        // Dragged by its title bar, it follows from the next frame.
        if (options.move) {
            const core::Interaction in = context.interact(core::hash_label("#MOVE", id), bar, {.beneath = true});
            if (in.pressed) window.move_from = rect.min;
            if (in.held && in.dragging) {
                const glm::vec2 to = window.move_from + (in.position - in.press_position);
                window.rect = {.min = to, .max = to + rect.size()};
                context.request_frame();
            }
        }
    }
    // What it holds stays below its title bar.
    dl.push_clip(rect.min + glm::vec2{0.0f, window.bar}, rect.max);
    k.window_stack.push_back(id);
    return true;
}

void end_window() {
    Kit& k = kit();
    if (k.window_stack.empty()) return;
    const Id id = k.window_stack.back();
    k.window_stack.pop_back();
    Window& window = k.windows[id];
    core::Context& context = ui();
    context.draw_list().pop_clip();
    if (window.options.scrollbar) scrollbar(window, id);
    context.end_panel();
    restore_item();
}

} // namespace corekit

using namespace corekit;

void next_window_place(glm::vec2 pos, glm::vec2 size) {
    kit().next_window.pos = pos;
    kit().next_window.size = size;
}

void next_window_size_once(glm::vec2 size) {
    kit().next_window.size_once = size;
}

void next_window_pos(glm::vec2 pos) {
    kit().next_window.pos = pos;
}

void next_window_size_limits(glm::vec2 min, glm::vec2 max) {
    kit().next_window.size_limits = std::pair{min, max};
}

WorkArea work_area() {
    return {.pos = {0.0f, 0.0f}, .size = ui().root_size()};
}

std::vector<ShownWindow> shown_windows() {
    std::vector<ShownWindow> shown;
    const uint64_t frame = ui().frame();
    for (const auto& [id, window] : kit().windows) {
        if (window.frame == frame) shown.push_back({window.name, window.rect.min, window.rect.size()});
    }
    return shown;
}

std::optional<ShownWindow> window_named(const std::string& name) {
    const auto& windows = kit().windows;
    const auto found = windows.find(core::hash_label(name, 0));
    if (found == windows.end()) return std::nullopt;
    return ShownWindow{found->second.name, found->second.rect.min, found->second.rect.size()};
}

InWindow::InWindow(const char* name) {
    const auto& windows = kit().windows;
    const auto found = windows.find(core::hash_label(name, 0));
    // Only a window drawn earlier this frame is drawn in again.
    if (found == windows.end() || found->second.frame != ui().frame()) return;
    visible_ = begin_window(name, found->second.options, found->second.padding);
}

InWindow::~InWindow() {
    if (visible_) end_window();
}

void focus_window(const char* name) {
    kit().focused_window = core::hash_label(name, 0);
}

glm::vec2 window_pos() {
    return ui().area_rect().min;
}

glm::vec2 window_size() {
    return ui().area_rect().size();
}

glm::vec2 window_content_min() {
    return ui().area_inside().min - ui().scroll();
}

glm::vec2 window_content_max() {
    return ui().area_inside().max - ui().scroll();
}

bool window_hovered() {
    return ui().area_under_pointer() && !ui().any_held();
}

bool window_or_child_hovered() {
    return ui().area_or_inner_under_pointer() && !ui().any_held();
}

bool window_under_mouse() {
    return ui().area_under_pointer();
}

bool window_focused() {
    const Kit& k = kit();
    return !k.window_stack.empty() && k.window_stack.back() == k.focused_window;
}

bool window_or_child_focused() {
    return window_focused();
}

float scroll_y() {
    return ui().scroll().y;
}

float scroll_max_y() {
    return ui().scroll_max().y;
}

void set_scroll_y(float y) {
    ui().set_scroll({ui().scroll().x, y});
}

void scroll_to_cursor(float ratio) {
    core::Context& context = ui();
    // The line the cursor ended, `ratio` of the way down the area: from its
    // top less the spacing above it to its bottom and the spacing below.
    const glm::vec2 scroll = context.scroll();
    const float inside = context.area_inside().min.y;
    const float spacing = kit().style.item_spacing.y;
    const float bottom = context.layout().cursor().y - inside + scroll.y;
    const float top = context.layout().last_item().min.y - inside + scroll.y;
    const float at = (top - spacing) + ((bottom - top) + spacing) * ratio;
    context.set_scroll({scroll.x, at - context.area_inside().height() * ratio});
}

bool begin_host_window(const char* name, WindowOptions options) {
    options.title_bar = false;
    options.resize = false;
    options.move = false;
    options.collapse = false;
    return begin_window(name, options, {0.0f, 0.0f});
}

void end_host_window() {
    end_window();
}

bool begin_plain_window(const char* name, glm::vec2 pos, glm::vec2 size) {
    next_window_place(pos, size);
    return begin_window(name,
                        {.title_bar = false,
                         .resize = false,
                         .move = false,
                         .collapse = false,
                         .scrollbar = false,
                         .to_front = false,
                         .remembered = false},
                        kit().style.window_padding);
}

void end_plain_window() {
    end_window();
}

} // namespace ftk::ui
