#include "ftk/ui/kit/dock.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/input.hpp"
#include "ftk/ui/kit/layout.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <utility>

// Docking as the kit keeps it on the core: a tree of splits whose leaves
// hold windows as tabs, laid out over the dockspace each frame. A split's
// splitter drags its share; a tab drags along its leaf's tabs to another
// place among them, or off them to tear its window out and carry it; a
// window carried by its title bar or its tab docks into the leaf whose
// tabs it is let go over. A leaf left empty folds into its parent, as
// ImGui's do, unless it is the dockspace's middle.

namespace ftk::ui {

using namespace corekit;

namespace {

// The room between the two parts of a split, as ImGui leaves it, and how
// far either side of it the splitter takes the pointer.
constexpr float SPLIT_GAP = 2.0f;
constexpr float SPLIT_REACH = 4.0f;
// The least a split's part keeps across, as ImGui's least window size.
constexpr float PART_LEAST = 32.0f;
// How far off its tabs, in lines of text, a tab is dragged to tear it
// out: down or up, and past either end, as ImGui's.
constexpr float TEAR_DOWN = 1.5f;
constexpr float TEAR_ALONG = 2.2f;

Id window_id(const std::string& name) {
    return core::hash_label(name, 0);
}

Id splitter_id(Id split) {
    return core::hash_label("#SPLIT", split);
}

// Drawn last frame or this one: a window that has a tab.
bool drawn_lately(const Window& window) {
    return window.frame != 0 && window.frame + 1 >= ui().frame();
}

// The pointer's shape over the splitter of `split`.
void splitter_cursor(const DockPart& split) {
    ui().set_cursor_shape(split.split == DockAxis::LeftRight ? MouseCursor::ResizeEW : MouseCursor::ResizeNS);
}

// The tabs across the top of a leaf at `rect`.
core::Rect tab_strip(core::Rect rect) {
    return {.min = rect.min, .max = {rect.max.x, rect.min.y + frame_height()}};
}

// The leaf `id`, left with no windows, folds into its parent: the other
// part takes the parent's place, keeping the parent's id, as ImGui merges
// them. The dockspace's middle stays, empty.
void fold_if_empty(Kit& k, Id id) {
    const DockPart& leaf = k.docks.at(id);
    if (!leaf.windows.empty() || leaf.middle || leaf.parent == 0) return;
    const Id parent_id = leaf.parent;
    DockPart& parent = k.docks.at(parent_id);
    const Id other_id = parent.children[0] == id ? parent.children[1] : parent.children[0];
    DockPart other = std::move(k.docks.at(other_id));
    k.docks.erase(id);
    k.docks.erase(other_id);
    parent.split = other.split;
    parent.ratio = other.ratio;
    parent.children = other.children;
    parent.windows = std::move(other.windows);
    parent.front = other.front;
    parent.middle = other.middle;
    for (const Id child : parent.children) {
        if (child != 0) k.docks.at(child).parent = parent_id;
    }
    for (const Id window : parent.windows) k.windows.at(window).dock = parent_id;
}

// The window `id` docked into the leaf `leaf`, as its tab in front.
void dock_into(Kit& k, Id id, Id leaf_id) {
    DockPart& leaf = k.docks.at(leaf_id);
    Window& window = k.windows.at(id);
    window.dock = leaf_id;
    if (!std::ranges::contains(leaf.windows, id)) leaf.windows.push_back(id);
    leaf.front = id;
    k.focused_window = id;
}

// The window `id` torn out of its leaf by its tab, `grab` from its tab's
// corner to the pointer: floating where the tab was, as big as the leaf,
// carried on by the pointer and in front of the rest.
void tear_out(Kit& k, Id id, glm::vec2 grab) {
    Window& window = k.windows.at(id);
    const Id leaf_id = window.dock;
    DockPart& leaf = k.docks.at(leaf_id);
    const glm::vec2 size = leaf.rect.size();
    const Id parent = leaf.parent;
    std::erase(leaf.windows, id);
    if (leaf.front == id) leaf.front = 0;
    window.dock = 0;
    const glm::vec2 at = mouse_pos() - grab;
    window.rect = {.min = at, .max = at + size};
    window.sized = true;
    ui().bring_to_front(id);
    ui().request_frame();
    fold_if_empty(k, leaf_id);
    // Its home is the leaf it left, or the parent that leaf folded into.
    k.carried = {.window = id, .grab = grab, .home = k.docks.contains(leaf_id) ? leaf_id : parent};
}

// The window `id`, let go past the edge of the OS window it was carried in
// at `pointer`, asks for an OS window of its own, as big as it is, near
// where it was let go; drawn there once the program opens it.
void tear_into_window(Kit& k, Window& window, Id id, glm::vec2 pointer) {
    core::Context& context = ui();
    window.root = core::hash_label("#ROOT", id);
    window.home = k.carried.home;
    context.open_root({.root = window.root,
                       .size = window.rect.size(),
                       .near = context.root(),
                       .at = pointer - k.carried.grab,
                       .title = std::string(shown(window.name))});
}

// The leaf, shown this frame, whose tabs are under `at`; 0 for none.
Id leaf_under(const Kit& k, glm::vec2 at) {
    for (const auto& [id, part] : k.docks) {
        if (part.split != DockAxis::None || shown_leaf(id) == nullptr) continue;
        if (tab_strip(part.rect).contains(at)) return id;
    }
    return 0;
}

// The node `id` and every node under it gone, their windows floating.
void remove_dock(Kit& k, Id id) {
    const auto found = k.docks.find(id);
    if (found == k.docks.end()) return;
    const DockPart part = std::move(found->second);
    k.docks.erase(found);
    for (const Id child : part.children) {
        if (child != 0) remove_dock(k, child);
    }
    for (const Id window : part.windows) k.windows[window].dock = 0;
}

// The node `id` laid out over `rect` this frame, and a split's parts over
// their shares of it. A leaf nothing is shown in is the dockspace's floor,
// unless it is the middle and what is under the dockspace shows through.
void lay_out(Kit& k, Id id, core::Rect rect, bool see_through) {
    DockPart& part = k.docks.at(id);
    part.rect = rect;
    part.frame = ui().frame();
    if (part.split == DockAxis::None) {
        const bool empty =
            std::ranges::none_of(part.windows, [&](Id window) { return drawn_lately(k.windows.at(window)); });
        if (empty && !(part.middle && see_through)) {
            ui().draw_list().rect_filled(rect.min, rect.max, faded(theme::surface_base()));
        }
        return;
    }
    const int axis = part.split == DockAxis::LeftRight ? 0 : 1;
    const float room = std::max(rect.max[axis] - rect.min[axis] - SPLIT_GAP, 0.0f);
    float cut = rect.min[axis] + std::round(room * part.ratio);
    const auto strip_at = [&](float at) {
        core::Rect strip = rect;
        strip.min[axis] = at - SPLIT_REACH;
        strip.max[axis] = at + SPLIT_GAP + SPLIT_REACH;
        return strip;
    };

    // Its splitter, dragged, moves the cut, each part keeping its least.
    const core::Interaction in = ui().interact(splitter_id(id), strip_at(cut), {.beneath = true});
    if (in.pressed) k.split_grab = in.position[axis] - cut;
    if (in.held && room > 0.0f) {
        const float least = std::min(PART_LEAST, room * 0.5f);
        cut = std::round(std::clamp(in.position[axis] - k.split_grab, rect.min[axis] + least,
                                    rect.min[axis] + room - least));
        part.ratio = (cut - rect.min[axis]) / room;
    }
    part.strip = strip_at(cut);
    core::Rect gap = rect;
    gap.min[axis] = cut;
    gap.max[axis] = cut + SPLIT_GAP;
    if (in.hovered || in.held) {
        splitter_cursor(part);
        ui().draw_list().rect_filled(gap.min, gap.max, faded(in.held ? theme::accent() : theme::accent_dim()));
    }

    core::Rect first = rect;
    core::Rect second = rect;
    first.max[axis] = cut;
    second.min[axis] = cut + SPLIT_GAP;
    lay_out(k, part.children[0], first, see_through);
    lay_out(k, part.children[1], second, see_through);
}

} // namespace

namespace corekit {

DockPart* shown_leaf(Id node) {
    Kit& k = kit();
    const auto found = k.docks.find(node);
    if (found == k.docks.end() || found->second.split != DockAxis::None) return nullptr;
    const DockPart* root = &found->second;
    while (root->parent != 0) root = &k.docks.at(root->parent);
    const uint64_t frame = ui().frame();
    const bool shown = root->frame == frame || (root->frame + 1 == frame && root->seen + 1 == frame);
    return shown ? &found->second : nullptr;
}

bool take_front(DockPart& leaf, Id id) {
    if (leaf.front == id) return true;
    const Kit& k = kit();
    const auto front = k.windows.find(leaf.front);
    const bool kept = std::ranges::contains(leaf.windows, leaf.front) && front != k.windows.end() &&
                      drawn_lately(front->second);
    if (kept) return false;
    leaf.front = id;
    return true;
}

std::size_t dock_tabs(Id node, core::Rect strip) {
    core::Context& context = ui();
    Kit& k = kit();
    DockPart& leaf = k.docks.at(node);
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(strip.min, strip.max, faded(theme::surface_sunken()));
    const bool focused = std::ranges::contains(leaf.windows, k.focused_window);
    draw_bar_line(strip, focused);

    // The windows drawn lately, in order, each as wide as its label.
    struct Tab {
        Id id{0};
        float x{0.0f};
        float width{0.0f};
    };
    std::vector<Tab> tabs;
    float x = strip.min.x;
    for (const Id id : leaf.windows) {
        const Window& window = k.windows.at(id);
        if (!drawn_lately(window)) continue;
        const float width = tab_width(shown(window.name), false);
        tabs.push_back({.id = id, .x = x, .width = width});
        x += width + k.style.item_inner_spacing.x;
    }

    std::optional<std::pair<Id, Id>> swap;
    std::optional<std::pair<Id, glm::vec2>> torn;
    const float line = text_style().size;
    dl.push_clip(strip.min, strip.max);
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const Tab& tab = tabs[i];
        const bool front = leaf.front == tab.id;
        const core::Rect rect{.min = {tab.x, strip.min.y}, .max = {tab.x + tab.width, strip.max.y}};
        const DrawnTab drawn = draw_tab(core::hash_label("#TAB", tab.id), rect,
                                        {.label = shown(k.windows.at(tab.id).name),
                                         .ink = text_colour(),
                                         .front = front,
                                         .focused = focused});
        // A press brings its window forward, with the keyboard.
        if (drawn.in.pressed && !front) {
            leaf.front = tab.id;
            k.focused_window = tab.id;
            context.request_frame();
        }
        if (!drawn.in.held || !drawn.in.dragging) continue;
        // Dragged off the tabs, it is torn out; along them, past the middle
        // of the tab beside it, it takes that one's place.
        const glm::vec2 p = drawn.in.position;
        const bool off = p.y < strip.min.y - line * TEAR_DOWN || p.y > strip.max.y + line * TEAR_DOWN ||
                         p.x < strip.min.x - line * TEAR_ALONG || p.x > strip.max.x + line * TEAR_ALONG;
        if (off) {
            torn = std::pair{tab.id, drawn.in.press_position - rect.min};
        } else if (i > 0 && p.x < tabs[i - 1].x + tabs[i - 1].width * 0.5f) {
            swap = std::pair{tab.id, tabs[i - 1].id};
        } else if (i + 1 < tabs.size() && p.x > tabs[i + 1].x + tabs[i + 1].width * 0.5f) {
            swap = std::pair{tab.id, tabs[i + 1].id};
        }
    }
    dl.pop_clip();

    if (swap) {
        const auto a = std::ranges::find(leaf.windows, swap->first);
        const auto b = std::ranges::find(leaf.windows, swap->second);
        std::iter_swap(a, b);
        context.request_frame();
    }
    // Last: the leaf may fold away with it.
    if (torn) tear_out(k, torn->first, torn->second);
    return tabs.size();
}

void dock_edges(Id node, core::Rect rect) {
    Kit& k = kit();
    for (Id at = k.docks.at(node).parent; at != 0; at = k.docks.at(at).parent) {
        const DockPart& split = k.docks.at(at);
        const core::Rect edge = split.strip.intersect(rect);
        if (edge.empty()) continue;
        const core::Interaction in = ui().interact(splitter_id(at), edge, {.beneath = true});
        if (in.hovered || in.held) splitter_cursor(split);
    }
}

bool carry(Window& window, Id id, const WindowOptions& options) {
    Kit& k = kit();
    if (k.carried.window != id) return false;
    core::Context& context = ui();
    const glm::vec2 pointer = mouse_pos();
    const glm::vec2 at = pointer - k.carried.grab;
    window.rect = {.min = at, .max = at + window.rect.size()};
    k.carried.target = options.docking ? leaf_under(k, pointer) : 0;
    if (mouse_down(MouseButton::Left)) {
        // Over a leaf's tabs, the leaf lights where it would dock.
        if (k.carried.target != 0) {
            const core::Rect target = k.docks.at(k.carried.target).rect;
            context.begin_panel(core::hash_label("#DOCK_PREVIEW", id), target,
                                {.padding = {0.0f, 0.0f}, .scroll = false, .layer = core::Layer::Tooltip});
            context.draw_list().rect_filled(target.min, target.max, theme::accent_dim() * glm::vec4{1.0f, 1.0f, 1.0f, 0.4f});
            context.end_panel();
        }
        context.request_frame();
        return false;
    }
    // Let go: docked over a leaf's tabs; past the edge of its OS window, torn
    // out into one of its own; else left floating where it is.
    const Id target = k.carried.target;
    const glm::vec2 room = context.root_size();
    const bool outside = pointer.x < 0.0f || pointer.y < 0.0f || pointer.x >= room.x || pointer.y >= room.y;
    if (target == 0 && outside && options.docking) tear_into_window(k, window, id, pointer);
    k.carried = {};
    if (target == 0) return false;
    dock_into(k, id, target);
    return true;
}

void dock_home(Window& window, Id id) {
    Kit& k = kit();
    if (window.root != 0) ui().close_root(window.root);
    window.root = 0;
    Id leaf = std::exchange(window.home, 0);
    const auto is_leaf = [&](Id at) { return k.docks.contains(at) && k.docks.at(at).split == DockAxis::None; };
    if (!is_leaf(leaf)) {
        // The leaf it came from is gone: the middle of a dockspace.
        leaf = 0;
        for (const auto& [at, part] : k.docks) {
            if (part.middle && part.split == DockAxis::None) leaf = at;
        }
    }
    if (leaf != 0) {
        dock_into(k, id, leaf);
    } else {
        // Nowhere to dock: floating in the program's window.
        window.rect = {.min = {60.0f, 60.0f}, .max = glm::vec2{60.0f} + window.rect.size()};
    }
    ui().request_frame();
}

bool torn_tab(Id id, core::Rect strip) {
    core::Context& context = ui();
    Kit& k = kit();
    const Window& window = k.windows.at(id);
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(strip.min, strip.max, faded(theme::surface_sunken()));
    draw_bar_line(strip, k.focused_window == id);
    const std::string_view label = shown(window.name);
    const float width = tab_width(label, false);
    (void)draw_tab(core::hash_label("#TAB", id), {.min = strip.min, .max = {strip.min.x + width, strip.max.y}},
                   {.label = label, .ink = text_colour(), .front = true, .focused = k.focused_window == id});

    // The button at the row's right end, its icon lit under the mouse.
    const float side = strip.height();
    const core::Rect button{.min = {strip.max.x - side, strip.min.y}, .max = strip.max};
    const core::Interaction in = context.interact(core::hash_label("#DOCK_BACK", id), button, {.beneath = true});
    if (in.hovered || in.held) dl.rect_filled(button.min, button.max, faded(theme::surface_hover()), k.style.frame_rounding);
    const glm::vec2 icon = measure(icon::dock_back);
    draw_text(button.min + (button.size() - icon) * 0.5f, in.hovered ? theme::text() : theme::text_secondary(),
              icon::dock_back);
    if (in.hovered) context.set_cursor_shape(MouseCursor::Hand);
    return in.clicked;
}

} // namespace corekit

void dockspace(Id id, DockspaceOptions options) {
    core::Context& context = ui();
    Kit& k = kit();
    const auto [found, made] = k.docks.try_emplace(id);
    // A dockspace made here is one leaf, its middle.
    if (made) found->second.middle = true;
    found->second.seen = context.frame();
    // Hidden, it keeps its layout and takes no room.
    if (options.hidden) return;
    const core::Rect rect = context.layout().next(glm::max(space_left(), glm::vec2{1.0f}));
    place_still(rect);
    lay_out(k, id, rect, options.see_through);
}

bool dock_exists(Id id) {
    return kit().docks.contains(id);
}

DockBuild::DockBuild(Id id, glm::vec2 size) : id_{id} {
    Kit& k = kit();
    remove_dock(k, id);
    DockPart& root = k.docks[id];
    root.middle = true;
    root.rect = {.min = {0.0f, 0.0f}, .max = size};
}

// Built as it goes: there is nothing left to finish.
DockBuild::~DockBuild() = default;

DockSplit split_dock(Id node, DockSide side, float ratio) {
    Kit& k = kit();
    const auto found = k.docks.find(node);
    if (found == k.docks.end() || found->second.split != DockAxis::None) return {0, 0};
    DockPart& part = found->second;
    const Id at_side = core::hash_int(1, node);
    const Id rest = core::hash_int(2, node);
    k.docks[at_side] = {.parent = node};
    // The rest keeps what was docked here, and the middle.
    k.docks[rest] = {.parent = node, .windows = std::move(part.windows), .front = part.front, .middle = part.middle};
    for (const Id window : k.docks[rest].windows) k.windows[window].dock = rest;

    const bool first = side == DockSide::Left || side == DockSide::Top;
    part.split = side == DockSide::Left || side == DockSide::Right ? DockAxis::LeftRight : DockAxis::TopBottom;
    part.ratio = first ? ratio : 1.0f - ratio;
    part.children = first ? std::array<Id, 2>{at_side, rest} : std::array<Id, 2>{rest, at_side};
    part.windows.clear();
    part.front = 0;
    part.middle = false;
    return {at_side, rest};
}

void dock_window(const std::string& name, Id node) {
    Kit& k = kit();
    const auto found = k.docks.find(node);
    if (found == k.docks.end() || found->second.split != DockAxis::None) return;
    const Id id = window_id(name);
    Window& window = k.windows[id];
    if (window.name.empty()) window.name = name;
    if (const auto was = k.docks.find(window.dock); was != k.docks.end()) std::erase(was->second.windows, id);
    window.dock = node;
    found->second.windows.push_back(id);
}

void place_dock_tree(Id node, const DockNode& tree) {
    // Every split before any window: splitting a node that holds windows
    // would leave them in neither part.
    std::vector<std::pair<const std::string*, Id>> docks;
    const auto place = [&](const auto& self, const DockNode& at, Id id) -> void {
        if (at.split != DockAxis::None && at.children.size() == 2) {
            const DockSide first = at.split == DockAxis::LeftRight ? DockSide::Left : DockSide::Top;
            const DockSplit parts = split_dock(id, first, at.ratio);
            self(self, at.children[0], parts.side);
            self(self, at.children[1], parts.rest);
        }
        for (const std::string& window : at.windows) docks.emplace_back(&window, id);
    };
    place(place, tree, node);
    for (const auto& [window, id] : docks) dock_window(*window, id);
}

void build_dock_tree(Id id, glm::vec2 size, const DockNode& tree) {
    DockBuild build(id, size);
    place_dock_tree(id, tree);
}

std::optional<DockNode> dock_tree(Id id, std::span<const std::string> windows) {
    const Kit& k = kit();
    if (!k.docks.contains(id)) return std::nullopt;

    // The windows asked about, whether they have been drawn yet or not.
    std::unordered_map<Id, const std::string*> asked;
    for (const std::string& name : windows) asked.emplace(window_id(name), &name);

    const auto read = [&](const auto& self, Id at) -> DockNode {
        const DockPart& part = k.docks.at(at);
        DockNode out{.split = part.split, .ratio = part.ratio};
        if (part.split != DockAxis::None) {
            out.children.push_back(self(self, part.children[0]));
            out.children.push_back(self(self, part.children[1]));
        }
        // In tab order.
        for (const Id window : part.windows) {
            if (const auto name = asked.find(window); name != asked.end()) out.windows.push_back(*name->second);
        }
        return out;
    };
    return read(read, id);
}

Id window_dock(const std::string& name) {
    const Kit& k = kit();
    const auto found = k.windows.find(window_id(name));
    return found != k.windows.end() && found->second.frame != 0 ? found->second.dock : 0;
}

} // namespace ftk::ui
