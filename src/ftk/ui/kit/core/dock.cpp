#include "ftk/ui/kit/dock.hpp"

#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/layout.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

// Docking as the kit keeps it on the core: a tree of splits whose leaves
// hold windows as tabs, laid out over the dockspace each frame. The splits
// keep the shares they were given: dragging their edges, moving tabs and
// tearing them off are the shell's (Phase 8 of the UI core plan).

namespace ftk::ui {

using namespace corekit;

namespace {

// The room between the two parts of a split, as ImGui leaves it.
constexpr float SPLIT_GAP = 2.0f;

Id window_id(const std::string& name) {
    return core::hash_label(name, 0);
}

// Drawn last frame or this one: a window that has a tab.
bool drawn_lately(const Window& window) {
    return window.frame != 0 && window.frame + 1 >= ui().frame();
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
    const float cut = rect.min[axis] + std::round(room * part.ratio);
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

    std::size_t count = 0;
    float x = strip.min.x;
    dl.push_clip(strip.min, strip.max);
    for (const Id id : leaf.windows) {
        const Window& window = k.windows.at(id);
        if (!drawn_lately(window)) continue;
        const std::string_view label = shown(window.name);
        const float width = tab_width(label, false);
        const bool front = leaf.front == id;
        const DrawnTab drawn =
            draw_tab(core::hash_label("#TAB", id), {.min = {x, strip.min.y}, .max = {x + width, strip.max.y}},
                     {.label = label, .ink = text_colour(), .front = front, .focused = focused});
        // A press brings its window forward, with the keyboard.
        if (drawn.in.pressed && !front) {
            leaf.front = id;
            k.focused_window = id;
            context.request_frame();
        }
        x += width + k.style.item_inner_spacing.x;
        ++count;
    }
    dl.pop_clip();
    return count;
}

} // namespace corekit

DockNode DockNode::left_right(float ratio, DockNode left, DockNode right) {
    DockNode node{.split = DockAxis::LeftRight, .ratio = ratio};
    node.children.push_back(std::move(left));
    node.children.push_back(std::move(right));
    return node;
}

DockNode DockNode::top_bottom(float ratio, DockNode top, DockNode bottom) {
    DockNode node{.split = DockAxis::TopBottom, .ratio = ratio};
    node.children.push_back(std::move(top));
    node.children.push_back(std::move(bottom));
    return node;
}

DockNode DockNode::tabs(std::initializer_list<std::string> windows) {
    return {.windows = windows};
}

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

    // Each window's node, whether it has been drawn yet or not.
    std::unordered_map<Id, std::vector<std::string>> docked;
    for (const std::string& name : windows) {
        const auto found = k.windows.find(window_id(name));
        if (found != k.windows.end() && found->second.dock != 0) docked[found->second.dock].push_back(name);
    }

    const auto read = [&](const auto& self, Id at) -> DockNode {
        const DockPart& part = k.docks.at(at);
        DockNode out{.split = part.split, .ratio = part.ratio};
        if (part.split != DockAxis::None) {
            out.children.push_back(self(self, part.children[0]));
            out.children.push_back(self(self, part.children[1]));
        }
        if (auto it = docked.find(at); it != docked.end()) out.windows = it->second;
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
