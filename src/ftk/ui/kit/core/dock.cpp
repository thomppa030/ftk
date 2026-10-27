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
// them, so what is left grows into the room it had. Only a dockspace's
// last leaf stays, empty.
void fold_if_empty(Kit& k, Id id) {
    const DockPart& leaf = k.docks.at(id);
    if (!leaf.windows.empty() || leaf.parent == 0) return;
    const bool was_middle = leaf.middle;
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
    // The middle too gives its room to what is left: it goes on in there.
    parent.middle = other.middle || was_middle;
    for (const Id child : parent.children) {
        if (child != 0) k.docks.at(child).parent = parent_id;
    }
    for (const Id window : parent.windows) k.windows.at(window).dock = parent_id;
}

// Whether any window docked in the node `id` has been drawn lately.
bool shows(const Kit& k, Id id) {
    const DockPart& part = k.docks.at(id);
    if (part.split != DockAxis::None) return shows(k, part.children[0]) || shows(k, part.children[1]);
    return std::ranges::any_of(part.windows, [&](Id window) { return drawn_lately(k.windows.at(window)); });
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
    std::erase(leaf.windows, id);
    if (leaf.front == id) leaf.front = 0;
    window.dock = 0;
    const glm::vec2 at = mouse_pos() - grab;
    window.rect = {.min = at, .max = at + size};
    window.sized = true;
    ui().bring_to_front(id);
    ui().request_frame();
    fold_if_empty(k, leaf_id);
    k.carried = {.window = id, .grab = grab};
}

// `window` moved the least way that puts it inside `room`, against the edge
// it went past, so its title bar is in reach; one bigger than the room has
// its top left corner in it.
void keep_inside(Window& window, glm::vec2 room) {
    const glm::vec2 size = window.rect.size();
    const glm::vec2 at = glm::max(glm::min(window.rect.min, room - size), glm::vec2{0.0f});
    window.rect = {.min = at, .max = at + size};
}

// The leaf, shown this frame, whose tabs are under `at`; 0 for none.
// A dock target's side, and the room between a leaf's five.
constexpr float TARGET = 32.0f;
constexpr float TARGET_GAP = 6.0f;
// A side drop's share of what it splits: the window's own size, within
// these, as ImGui sizes the new part by the window docked.
constexpr float SIDE_LEAST = 0.2f;
constexpr float SIDE_MOST = 0.5f;

DockSide side_of(DockDrop drop) {
    switch (drop) {
    case DockDrop::Left: return DockSide::Left;
    case DockDrop::Right: return DockSide::Right;
    case DockDrop::Top: return DockSide::Top;
    default: return DockSide::Bottom;
    }
}

struct Target {
    DockDrop drop{DockDrop::None};
    core::Rect rect{};
};

core::Rect square(glm::vec2 centre) {
    return {.min = centre - glm::vec2{TARGET * 0.5f}, .max = centre + glm::vec2{TARGET * 0.5f}};
}

// A leaf's five targets about its middle, as ImGui lays them out: its tabs
// in the middle, its sides around them. None for a leaf too small to hold
// them.
std::vector<Target> leaf_targets(core::Rect rect) {
    const float step = TARGET + TARGET_GAP;
    if (rect.width() < step * 3.0f || rect.height() < step * 3.0f) return {};
    const glm::vec2 c = rect.centre();
    return {{DockDrop::Tabs, square(c)},
            {DockDrop::Left, square(c - glm::vec2{step, 0.0f})},
            {DockDrop::Right, square(c + glm::vec2{step, 0.0f})},
            {DockDrop::Top, square(c - glm::vec2{0.0f, step})},
            {DockDrop::Bottom, square(c + glm::vec2{0.0f, step})}};
}

// A dockspace's four targets, at the middle of each of its edges, which
// split the whole of it.
std::vector<Target> root_targets(core::Rect rect) {
    const glm::vec2 c = rect.centre();
    const float in = TARGET * 0.5f + TARGET_GAP;
    return {{DockDrop::Left, square({rect.min.x + in, c.y})},
            {DockDrop::Right, square({rect.max.x - in, c.y})},
            {DockDrop::Top, square({c.x, rect.min.y + in})},
            {DockDrop::Bottom, square({c.x, rect.max.y - in})}};
}

// The share of `rect` a window `size` big takes, dropped at its side `drop`.
float drop_share(core::Rect rect, DockDrop drop, glm::vec2 size) {
    const int axis = drop == DockDrop::Left || drop == DockDrop::Right ? 0 : 1;
    const float across = rect.max[axis] - rect.min[axis];
    return std::clamp(size[axis] / std::max(across, 1.0f), SIDE_LEAST, SIDE_MOST);
}

// The part of `rect` a window `size` big takes, dropped at `drop`.
core::Rect drop_rect(core::Rect rect, DockDrop drop, glm::vec2 size) {
    if (drop == DockDrop::Tabs || drop == DockDrop::None) return rect;
    const int axis = drop == DockDrop::Left || drop == DockDrop::Right ? 0 : 1;
    const float across = rect.max[axis] - rect.min[axis];
    const float share = drop_share(rect, drop, size);
    core::Rect part = rect;
    if (drop == DockDrop::Left || drop == DockDrop::Top) {
        part.max[axis] = rect.min[axis] + std::round(across * share);
    } else {
        part.min[axis] = rect.max[axis] - std::round(across * share);
    }
    return part;
}

// What a window carried to `at` drops on: a dockspace's edge target, a
// target of the leaf under it, or that leaf's tabs; and the dockspace and
// leaf it is over, to show their targets.
struct Over {
    Id root{0};
    Id leaf{0};
    Id target{0};
    DockDrop drop{DockDrop::None};
};

Over over_dock(const Kit& k, glm::vec2 at) {
    Over over;
    // The dockspaces of the OS window being drawn, where `at` is.
    const core::RootId window = ui().root();
    for (const auto& [id, part] : k.docks) {
        if (part.parent != 0 || part.window != window || !part.rect.contains(at)) continue;
        // A dockspace shown this frame: its leaves are.
        Id leaf = id;
        while (k.docks.at(leaf).split != DockAxis::None) {
            const DockPart& split = k.docks.at(leaf);
            leaf = k.docks.at(split.children[0]).rect.contains(at) ? split.children[0] : split.children[1];
        }
        if (shown_leaf(leaf) == nullptr) continue;
        over.root = id;
        over.leaf = leaf;
        for (const Target& target : root_targets(part.rect)) {
            if (target.rect.contains(at)) return {.root = id, .leaf = leaf, .target = id, .drop = target.drop};
        }
        const DockPart& under = k.docks.at(leaf);
        for (const Target& target : leaf_targets(under.rect)) {
            if (target.rect.contains(at)) return {.root = id, .leaf = leaf, .target = leaf, .drop = target.drop};
        }
        if (tab_strip(under.rect).contains(at)) return {.root = id, .leaf = leaf, .target = leaf, .drop = DockDrop::Tabs};
        return over;
    }
    return over;
}

// A target drawn as ImGui draws one: a plate, and in it the shape of what
// it does, the part a drop there takes lit; `lit` while the pointer is on it.
void draw_target(core::DrawList& dl, const Target& target, bool lit) {
    const core::Rect& r = target.rect;
    dl.rect_filled(r.min, r.max, theme::surface_raised(), 4.0f);
    dl.rect(r.min, r.max, theme::border(), 4.0f);
    const core::Rect inner{.min = r.min + glm::vec2{6.0f}, .max = r.max - glm::vec2{6.0f}};
    dl.rect(inner.min, inner.max, theme::text_secondary(), 2.0f);
    const core::Rect part = drop_rect(inner, target.drop, inner.size() * 0.5f);
    dl.rect_filled(part.min, part.max, lit ? theme::accent() : theme::accent_dim(), 2.0f);
}

// While a window `size` big is carried over a dockspace: its edge targets,
// the targets of the leaf under the pointer, and the part the window would
// take lit, over everything else.
void draw_drop_targets(const Kit& k, const Over& over, Id window, glm::vec2 size) {
    if (over.root == 0) return;
    core::Context& context = ui();
    const core::Rect area = k.docks.at(over.root).rect;
    context.begin_panel(core::hash_label("#DOCK_TARGETS", window), area,
                        {.padding = {0.0f, 0.0f}, .scroll = false, .layer = core::Layer::Tooltip});
    core::DrawList& dl = context.draw_list();
    if (over.drop != DockDrop::None) {
        const core::Rect taken = drop_rect(k.docks.at(over.target).rect, over.drop, size);
        dl.rect_filled(taken.min, taken.max, theme::accent_dim() * glm::vec4{1.0f, 1.0f, 1.0f, 0.35f});
        dl.rect(taken.min, taken.max, theme::accent(), 0.0f, RoundCorners::All, 2.0f);
    }
    for (const Target& target : leaf_targets(k.docks.at(over.leaf).rect)) {
        draw_target(dl, target, over.target == over.leaf && over.drop == target.drop);
    }
    for (const Target& target : root_targets(area)) {
        draw_target(dl, target, over.target == over.root && over.drop == target.drop);
    }
    context.end_panel();
}

// The node `id` split for a window dropped at its side `side`, the window's
// part `share` of it: what it held moves into the other part, keeping the
// middle, and the node keeps its place and id. Returns the new leaf.
Id split_for_drop(Kit& k, Id id, DockSide side, float share) {
    // Ids of their own, apart from those split_dock() gives a tree it builds.
    const auto fresh = [&] {
        Id made = 0;
        do {
            made = core::hash_int(++k.dock_serial, core::hash_label("#DROP", id));
        } while (made == 0 || k.docks.contains(made));
        return made;
    };
    const Id rest = fresh();
    const Id at_side = fresh();
    DockPart& part = k.docks.at(id);
    DockPart moved{.parent = id,
                   .split = part.split,
                   .ratio = part.ratio,
                   .children = part.children,
                   .windows = std::move(part.windows),
                   .front = part.front,
                   .middle = part.middle,
                   .rect = part.rect,
                   .frame = part.frame};
    for (const Id child : moved.children) {
        if (child != 0) k.docks.at(child).parent = rest;
    }
    for (const Id window : moved.windows) k.windows.at(window).dock = rest;
    k.docks[rest] = std::move(moved);
    k.docks[at_side] = {.parent = id, .rect = part.rect, .frame = part.frame};

    DockPart& split = k.docks.at(id);
    const bool first = side == DockSide::Left || side == DockSide::Top;
    split.split = side == DockSide::Left || side == DockSide::Right ? DockAxis::LeftRight : DockAxis::TopBottom;
    split.ratio = first ? share : 1.0f - share;
    split.children = first ? std::array<Id, 2>{at_side, rest} : std::array<Id, 2>{rest, at_side};
    split.windows.clear();
    split.front = 0;
    split.middle = false;
    return at_side;
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
        if (!shows(k, id) && !(part.middle && see_through)) {
            ui().draw_list().rect_filled(rect.min, rect.max, faded(theme::surface_base()));
        }
        return;
    }
    // A part none of whose windows is drawn, closed for now, lends all its
    // room to the other and keeps its place for when they are drawn again,
    // as ImGui's do.
    const bool first_shows = shows(k, part.children[0]);
    const bool second_shows = shows(k, part.children[1]);
    if (first_shows != second_shows) {
        part.strip = {};
        lay_out(k, first_shows ? part.children[0] : part.children[1], rect, see_through);
        lay_out(k, first_shows ? part.children[1] : part.children[0], {.min = rect.min, .max = rect.min}, see_through);
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
        const float width = tab_width(shown(window.name), window.closable);
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
                                         .closable = k.windows.at(tab.id).closable,
                                         .focused = focused});
        // Its × closes its window as that is begun, which holds the flag.
        if (drawn.closed) {
            k.windows.at(tab.id).close_asked = true;
            context.request_frame();
        }
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

bool docked_alone(Id node, Id window) {
    const Kit& k = kit();
    const DockPart& leaf = k.docks.at(node);
    return std::ranges::none_of(leaf.windows,
                                [&](Id other) { return other != window && drawn_lately(k.windows.at(other)); });
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

std::optional<DockPlace> carry_tab_over_docks(Id tab, glm::vec2 at, float share) {
    const Kit& k = kit();
    const Over over = over_dock(k, at);
    // As big as the share it takes of the part under it, beside it.
    const glm::vec2 size = over.target != 0 ? k.docks.at(over.target).rect.size() * share : glm::vec2{0.0f};
    draw_drop_targets(k, over, tab, size);
    if (over.drop == DockDrop::None) return std::nullopt;
    if (over.drop == DockDrop::Tabs) return DockPlace{.node = over.target};
    return DockPlace{.node = over.target,
                     .side = side_of(over.drop),
                     .share = drop_share(k.docks.at(over.target).rect, over.drop, size)};
}

bool carry(Window& window, Id id, const WindowOptions& options) {
    Kit& k = kit();
    if (k.carried.window != id) return false;
    core::Context& context = ui();
    // Let go, where it came up: past the window's edge the platform holds
    // the pointer at that edge after it.
    const bool held = mouse_down(MouseButton::Left);
    const glm::vec2 pointer = held ? mouse_pos() : mouse_release_pos();
    const glm::vec2 at = pointer - k.carried.grab;
    const glm::vec2 size = window.rect.size();
    window.rect = {.min = at, .max = at + size};
    const Over over = options.docking ? over_dock(k, pointer) : Over{};
    k.carried.target = over.target;
    k.carried.drop = over.drop;
    k.carried.lands_on_rows = options.lands_on_rows;
    k.carried.root = context.root();
    k.carried.at = pointer;
    k.carried.frame = context.frame();
    const glm::vec2 room = context.root_size();
    const bool outside = pointer.x < 0.0f || pointer.y < 0.0f || pointer.x >= room.x || pointer.y >= room.y;
    if (held) {
        // Over a dockspace, its targets show, and the part it would take.
        draw_drop_targets(k, over, id, size);
        // Past the edge of its OS window, one that lands on rows shows the
        // copy a tab carried there shows: let go, it opens in its own, unless
        // a row of another window parted for it.
        if (options.lands_on_rows && outside) {
            const bool over_a_row = k.carried.over_row_frame + 1 >= context.frame();
            draw_tab_copy(shown(window.name), nullptr, {}, !over_a_row);
        }
        context.request_frame();
        return false;
    }
    // Let go: docked at the target it is over; on a row of tabs that takes
    // panels, landed there; past the edge of its OS window, floating inside
    // it against that edge; else left floating where it is.
    const Id target = k.carried.target;
    const DockDrop drop = k.carried.drop;
    // On a row of tabs that takes panels, it lands there, floating where it
    // is until the program shows it as a tab.
    if (target == 0 && options.lands_on_rows && land_panel(id, pointer)) {
        k.carried = {};
        return false;
    }
    if (target == 0 && outside) {
        // One that lands on rows is for the program to show in a window of
        // its own there; it stays inside meanwhile.
        if (options.lands_on_rows) window.torn_off = pointer;
        keep_inside(window, room);
    }
    k.carried = {};
    if (target == 0) return false;
    if (drop == DockDrop::Tabs) {
        dock_into(k, id, target);
    } else {
        const float share = drop_share(k.docks.at(target).rect, drop, size);
        dock_into(k, id, split_for_drop(k, target, side_of(drop), share));
    }
    return true;
}

} // namespace corekit

namespace {

// The window that has the keyboard, drawn lately and taking keys of its own;
// null when none has.
const Window* keyboard_holder(const Kit& k) {
    const auto holder = k.windows.find(k.focused_window);
    if (holder == k.windows.end()) return nullptr;
    const Window& window = holder->second;
    if (window.frame + 1 < ui().frame() || !window.options.nav_focus) return nullptr;
    return &window;
}

// Whether `window` is docked in the dockspace `id`: up from its leaf to it.
bool docked_in(const Kit& k, const Window& window, Id id) {
    for (Id part = window.dock; part != 0;) {
        if (part == id) return true;
        const auto node = k.docks.find(part);
        if (node == k.docks.end()) break;
        part = node->second.parent;
    }
    return false;
}

} // namespace

bool keyboard_in_dockspace(Id id) {
    const Kit& k = kit();
    const Window* holder = keyboard_holder(k);
    return holder == nullptr || docked_in(k, *holder, id);
}

bool keyboard_docked_in(Id id) {
    const Kit& k = kit();
    const Window* holder = keyboard_holder(k);
    return holder != nullptr && docked_in(k, *holder, id);
}

void dockspace(Id id, DockspaceOptions options) {
    core::Context& context = ui();
    Kit& k = kit();
    const auto [found, made] = k.docks.try_emplace(id);
    // A dockspace made here is one leaf, its middle.
    if (made) found->second.middle = true;
    found->second.seen = context.frame();
    found->second.window = context.root();
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

bool dock_window_beside(const std::string& name, Id node, DockSide side, float share) {
    Kit& k = kit();
    if (!k.docks.contains(node)) return false;
    const Id id = window_id(name);
    Window& window = k.windows[id];
    if (window.name.empty()) window.name = name;
    if (k.docks.contains(window.dock)) return false;
    dock_into(k, id, split_for_drop(k, node, side, share));
    return true;
}

bool dock_window_at(const std::string& name, const DockPlace& place) {
    Kit& k = kit();
    const auto target = k.docks.find(place.node);
    if (target == k.docks.end()) return false;
    // Among the tabs of a leaf only.
    if (!place.side && target->second.split != DockAxis::None) return false;
    const Id id = window_id(name);
    Window& window = k.windows[id];
    if (window.name.empty()) window.name = name;
    if (!place.side && window.dock == place.node) {
        dock_into(k, id, place.node);
        return true;
    }
    Id node = place.node;
    // Out of where it was docked. Left empty, that part folds into its
    // parent with the part beside it, which then is the parent.
    if (const Id was = window.dock; k.docks.contains(was)) {
        DockPart& leaf = k.docks.at(was);
        std::erase(leaf.windows, id);
        if (leaf.front == id) leaf.front = 0;
        window.dock = 0;
        if (leaf.windows.empty() && leaf.parent != 0) {
            const DockPart& parent = k.docks.at(leaf.parent);
            const Id beside = parent.children[0] == was ? parent.children[1] : parent.children[0];
            if (node == was || node == beside) node = leaf.parent;
            fold_if_empty(k, was);
        }
    }
    dock_into(k, id, place.side ? split_for_drop(k, node, *place.side, place.share) : node);
    return true;
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
