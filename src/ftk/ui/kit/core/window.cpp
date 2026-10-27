#include "ftk/ui/kit/window.hpp"

#include "ftk/ui/core_kit.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
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

// A title bar's words from `at`: the icon in its colour, the name in bold,
// then the detail in the secondary colour.
void draw_title(const WindowTitle& title, glm::vec2 at, std::string_view name) {
    const float gap = kit().style.item_inner_spacing.x;
    if (title.icon != nullptr) {
        draw_text(at, title.icon_colour, title.icon);
        at.x += measure(title.icon).x + gap;
    }
    {
        FontScope bold(Font::Bold, 0.0f);
        draw_text(at, theme::text(), name);
        at.x += measure(name).x + gap;
    }
    if (!title.detail.empty()) draw_text(at, theme::text_secondary(), title.detail);
}

// The window's scrollbar down its right edge, while what it holds is
// taller than it. A window no taller than its title bar, as one is before
// it has its size, has no room for one.
void scrollbar(const Window& window, Id id) {
    core::Context& context = ui();
    const core::Rect& rect = window.rect;
    const core::Rect track{.min = {rect.max.x - kit().style.scrollbar_size, rect.min.y + window.bar}, .max = rect.max};
    float offset = context.scroll().y;
    if (scrollbar_at(core::hash_label("#SCROLLY", id), track, offset, track.height(), context.scroll_max().y)) {
        context.set_scroll({context.scroll().x, offset});
    }
}

// A window docked in a leaf of a dockspace: the leaf, its tabs across the
// top where a title bar would be, while it is the one in front; not shown
// while another is or while its dockspace isn't shown. It stays behind the
// windows that float, as ImGui's docked windows do.
bool begin_docked(Window& window, Id id, const WindowOptions& options, glm::vec2 padding, bool* open) {
    core::Context& context = ui();
    Kit& k = kit();
    k.window_stack.push_back(id);
    DockPart* leaf = shown_leaf(window.dock);
    window.shown = leaf != nullptr && take_front(*leaf, id);
    if (!window.shown) return false;

    window.rect = leaf->rect;
    // Its leaf lent its room while it wasn't drawn: it gets it back as the
    // dockspace is next laid out.
    if (window.rect.empty()) context.request_frame();
    window.padding = padding;
    // Alone in its leaf without a tab, it takes the leaf whole.
    const bool tabs = options.dock_tab || !docked_alone(window.dock, id);
    window.bar = tabs ? frame_height() : 0.0f;
    context.begin_panel(id, window.rect, {.padding = padding, .bar = window.bar, .raise = false});
    if (any_button_pressed() && context.area_or_inner_under_pointer()) k.focused_window = id;
    const core::Rect& rect = window.rect;
    // The splitters along its edges first, over what it holds.
    dock_edges(window.dock, rect);
    core::DrawList& dl = context.draw_list();
    if (options.background) dl.rect_filled(rect.min, rect.max, faded(theme::surface_base()));
    window.tabs = tabs ? dock_tabs(window.dock, {.min = rect.min, .max = {rect.max.x, rect.min.y + window.bar}}) : 1;
    if (open != nullptr && std::exchange(window.close_asked, false)) *open = false;
    dl.push_clip(rect.min + glm::vec2{0.0f, window.bar}, rect.max);
    return true;
}

} // namespace

bool begin_window(const char* name, const WindowOptions& options, glm::vec2 padding, bool* open) {
    core::Context& context = ui();
    Kit& k = kit();
    keep_item();
    const Id id = window_id(name);
    Window& window = k.windows[id];
    window.closable = open != nullptr;
    // Its tab's × pressed while it was behind another's: closed now.
    if (open != nullptr && std::exchange(window.close_asked, false)) *open = false;
    // A window carried but no longer drawn is let go of.
    if (k.carried.window != 0 && k.carried.window != id && !mouse_down(MouseButton::Left)) {
        const auto carried = k.windows.find(k.carried.window);
        if (carried == k.windows.end() || carried->second.frame + 1 < context.frame()) k.carried = {};
    }
    const bool again = window.frame == context.frame();
    if (again && !window.shown) {
        k.window_stack.push_back(id);
        return false;
    }
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
    // Placed by the settings file, it opens where that had it.
    const bool first = window.frame == 0 && !window.placed;
    if (window.frame == 0) window.name = name;
    if (first) window.rect = {.min = FIRST_PLACE, .max = FIRST_PLACE};
    const NextWindow next = std::exchange(k.next_window, {});
    if (options.docking && window.dock != 0 && k.docks.contains(window.dock)) {
        window.frame = context.frame();
        window.options = options;
        return begin_docked(window, id, options, padding, open);
    }
    window.shown = true;
    window.tabs = 0;
    if (next.pos_once && first) window.rect = {.min = *next.pos_once, .max = *next.pos_once};
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
    // Carried, it follows the pointer; let go over a leaf's tabs, it docks.
    if (carry(window, id, options)) return begin_docked(window, id, options, padding, open);

    context.begin_panel(id, window.rect,
                        {.padding = padding, .bar = window.bar, .fit = !window.sized, .raise = options.to_front});
    // A window that fits what it holds is as big as that came to.
    window.rect = context.panel_rect();
    const core::Rect& rect = window.rect;

    // Over the windows brought forward while it was away, as ImGui brings
    // forward a window that appears.
    if (appearing && options.to_front) context.bring_to_front(id);
    if ((appearing && options.nav_focus) || (any_button_pressed() && context.area_or_inner_under_pointer())) {
        k.focused_window = id;
    }

    core::DrawList& dl = context.draw_list();
    if (options.background) dl.rect_filled(rect.min, rect.max, faded(theme::surface_base()));
    if (window.bar > 0.0f) {
        const core::Rect bar{.min = rect.min, .max = {rect.max.x, rect.min.y + window.bar}};
        dl.rect_filled(bar.min, bar.max, faded(theme::surface_sunken()));
        // A × at its right end closes it, as ImGui's.
        float title_right = bar.max.x;
        if (open != nullptr) {
            const float side = text_style().size;
            const glm::vec2 at{bar.max.x - k.style.frame_padding.x - side, bar.min.y + k.style.frame_padding.y};
            if (close_button(core::hash_label("#CLOSE", id), at)) *open = false;
            title_right = at.x - k.style.item_inner_spacing.x;
        }
        dl.push_clip(bar.min, {title_right, bar.max.y});
        if (next.title) {
            draw_title(*next.title, bar.min + k.style.frame_padding, shown(name));
        } else {
            draw_text(bar.min + k.style.frame_padding, theme::text(), shown(name));
        }
        dl.pop_clip();
        // Dragged by its title bar, it is carried from the next frame. Not
        // on the frame it is let go, when carry() has just let go of it.
        if (options.move) {
            const core::Interaction in = context.interact(core::hash_label("#MOVE", id), bar, {.beneath = true});
            if (in.held && in.dragging && !in.released && k.carried.window != id) {
                k.carried = {.window = id, .grab = in.press_position - rect.min};
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
    if (window.shown) {
        core::Context& context = ui();
        context.draw_list().pop_clip();
        if (window.options.scrollbar) scrollbar(window, id);
        context.end_panel();
    }
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

void next_window_title(const char* icon, Color icon_colour, std::string_view detail) {
    kit().next_window.title = WindowTitle{.icon = icon, .icon_colour = icon_colour, .detail = std::string(detail)};
}

void next_window_pos(glm::vec2 pos) {
    kit().next_window.pos = pos;
}

void next_window_size_limits(glm::vec2 min, glm::vec2 max) {
    kit().next_window.size_limits = std::pair{min, max};
}

WorkArea work_area() {
    const core::Rect rect = work_rect();
    return {.pos = rect.min, .size = rect.size()};
}

float pixels_per_unit() {
    return ui().root_scale();
}

std::vector<ShownWindow> shown_windows() {
    std::vector<ShownWindow> shown;
    const uint64_t frame = ui().frame();
    for (const auto& [id, window] : kit().windows) {
        if (window.frame == frame && window.shown) shown.push_back({window.name, window.rect.min, window.rect.size()});
    }
    return shown;
}

std::optional<ShownWindow> window_named(const std::string& name) {
    const auto& windows = kit().windows;
    const auto found = windows.find(core::hash_label(name, 0));
    if (found == windows.end()) return std::nullopt;
    return ShownWindow{found->second.name, found->second.rect.min, found->second.rect.size()};
}

bool window_shown(const std::string& name) {
    Kit& k = kit();
    const Id id = core::hash_label(name, 0);
    const auto found = k.windows.find(id);
    if (found == k.windows.end()) return false;
    const Window& window = found->second;
    if (window.frame == 0 || window.frame + 1 < ui().frame()) return false;
    // Docked, as its part is now: a tab pressed or focus_window() since it
    // was drawn already counts.
    if (window.options.docking && k.docks.contains(window.dock)) {
        const DockPart* leaf = shown_leaf(window.dock);
        return leaf != nullptr && leaf->front == id;
    }
    return window.shown;
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
    Kit& k = kit();
    const Id id = core::hash_label(name, 0);
    k.focused_window = id;
    // Docked behind another tab, it comes forward, as ImGui's does.
    const auto found = k.windows.find(id);
    if (found == k.windows.end()) return;
    if (const auto leaf = k.docks.find(found->second.dock); leaf != k.docks.end()) leaf->second.front = id;
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
