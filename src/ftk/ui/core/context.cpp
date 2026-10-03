#include "ftk/ui/core/context.hpp"

#include "ftk/base/log.hpp"

#include <algorithm>
#include <cassert>

namespace ftk::ui::core {

namespace {

Context* current_context = nullptr;

// Frames something may go undrawn before what was kept for it is dropped.
constexpr uint64_t KEEP_FRAMES = 300;

} // namespace

Context::Context(text::AtlasOptions atlas) : text_{atlas} {}

Context::~Context() {
    if (current_context == this) current_context = nullptr;
}

void Context::begin_frame(double time) {
    if (in_frame_) {
        FTK_CORE_ERROR("UI: a frame began before the last one ended");
        (void)end_frame();
    }
    delta_ = frame_ == 0 ? 0.0f : static_cast<float>(time - time_);
    time_ = time;
    ++frame_;
    in_frame_ = true;
    typing_as_begun_ = typing();
    next_now_ = false;
    next_at_.reset();
    output_.roots.clear();
    output_.next = {};
    output_.open.clear();
    output_.close.clear();
    last_widget_ = 0;
    last_interaction_ = {};
    hit_sequence_ = 0;
    any_hovered_ = false;
    hover_next_ = 0;
    drag_.seen = false;
    disabled_.clear();
    text_.begin_frame();
    // A frame that takes input is followed by another: what a click or a
    // key changes partway through a frame, the parts drawn before it show
    // only when drawn again.
    const bool input = !events_.empty() || !key_events_.empty() || !typed_next_.empty() || composition_next_;
    take_input();
    take_keys();
    if (input) request_frame();
    // Told it had the keyboard, then never drawn again: nothing to tell.
    if (told_focused_.size() > 4) told_focused_.erase(told_focused_.begin(), told_focused_.end() - 2);
    focus_seen_ = false;
    focus_given_ = false;
    focus_order_.clear();
    focus_scopes_.clear();
    if (frame_ % KEEP_FRAMES == 0) sweep();
}

void Context::sweep() {
    const auto stale = [&](uint64_t frame) { return frame + KEEP_FRAMES < frame_; };
    std::erase_if(containers_, [&](const auto& kept) { return stale(kept.second.frame); });
    std::erase_if(scrolls_, [&](const auto& kept) { return stale(kept.second.frame); });
    std::erase_if(gestures_, [&](const auto& kept) { return stale(kept.second.frame); });
    for (auto& [id, root] : roots_) {
        std::erase_if(root->panels, [&](const auto& panel) { return stale(panel->frame); });
    }
}

void Context::begin_root(RootId root, glm::vec2 size, float scale) {
    assert(in_frame_ && "UI: a root begun outside a frame");
    if (root_ != nullptr) {
        FTK_CORE_ERROR("UI: window {} began inside window {}", root, root_->id);
        end_root();
    }
    std::unique_ptr<Root>& kept = roots_[root];
    if (!kept) kept = std::make_unique<Root>();
    root_ = kept.get();
    root_->id = root;
    root_->size = size;
    root_->scale = scale;
    // Drawn into already this frame, as another window was drawn: what it
    // holds goes on.
    if (root_->begun != frame_) {
        root_->begun = frame_;
        root_->cursor = MouseCursor::Arrow;
        root_->text_input = false;
        root_->list.reset(root_->draw, size, scale, &text_);
        root_->base_hits.clear();
        root_->areas_next.clear();
    }
    hit_targets_.assign(1, &root_->base_hits);
    panel_stack_.clear();
    layout_.reset();
    layout_.begin_area({.min = {0.0f, 0.0f}, .max = size});
    areas_.clear();
    open_containers_.clear();
    hidden_ = 0;
}

void Context::end_root() {
    if (root_ == nullptr) {
        FTK_CORE_ERROR("UI: a window ended that never began");
        return;
    }
    // What was left open is closed, so the next window starts clean.
    if (!open_containers_.empty() || !areas_.empty()) {
        FTK_CORE_ERROR("UI: window {} ended with {} container(s) and {} panel(s) or region(s) open", root_->id,
                       open_containers_.size(), areas_.size());
        while (!open_containers_.empty()) end_container();
        while (!areas_.empty()) areas_.back().panel ? end_panel() : end_region();
    }
    (void)layout_.end_area();
    // Ids pushed and never popped would shift every id after them.
    if (ids_.depth() != 0) {
        FTK_CORE_ERROR("UI: window {} ended with {} id(s) still pushed", root_->id, ids_.depth());
        ids_.clear();
    }

    // What each panel held, for fitting it next frame; a change is drawn
    // then.
    for (const auto& panel : root_->panels) {
        if (panel->frame != frame_) continue;
        if (panel->measured && panel->content != panel->content_next) request_frame();
        panel->content = panel->content_next;
        panel->measured = true;
    }

    // The panels drawn this frame go over the root's own shapes, layer by
    // layer and in order within one, and so do the places their widgets
    // take the pointers. One with nothing to fit yet waits a frame.
    std::vector<Panel*> drawn;
    for (const auto& panel : root_->panels) {
        if (panel->frame == frame_ && !panel->hidden) drawn.push_back(panel.get());
    }
    std::ranges::sort(drawn, [](const Panel* a, const Panel* b) {
        return a->layer != b->layer ? a->layer < b->layer : a->order < b->order;
    });
    order_hits(root_->base_hits);
    root_->hits = root_->base_hits;
    for (const Panel* panel : drawn) {
        root_->draw.primitives.insert(root_->draw.primitives.end(), panel->primitives.begin(), panel->primitives.end());
        root_->hits.insert(root_->hits.end(), panel->hits.begin(), panel->hits.end());
    }
    std::swap(root_->areas, root_->areas_next);

    // Hover found by last frame's places: if this frame's put something else
    // on top under a pointer, the next frame shows it.
    for (const Tracked& tracked : pointers_) {
        const Pointer& p = tracked.pointer;
        if (p.root != root_->id || !p.present) continue;
        const Hit* top = top_hit(root_->hits, p.now.position);
        if ((top != nullptr ? top->id : 0) != tracked.top) request_frame();
    }

    output_.roots.push_back({.root = root_->id,
                             .size = root_->size,
                             .scale = root_->scale,
                             .draw = &root_->draw,
                             .cursor = root_->cursor,
                             .text_input = root_->text_input,
                             .text_area = root_->text_area});
    root_ = nullptr;
    hit_targets_.clear();
}

glm::vec2 Context::root_size() const {
    return root_ != nullptr ? root_->size : glm::vec2{0.0f};
}

RootId Context::root() const {
    return root_ != nullptr ? root_->id : 0;
}

void Context::forget_root(RootId root) {
    if (root_ != nullptr && root_->id == root) {
        FTK_CORE_ERROR("UI: window {} forgotten while it is drawn", root);
        return;
    }
    roots_.erase(root);
}

bool Context::begin_other_root(RootId root) {
    assert(root_ != nullptr && "UI: another window drawn into outside a window; call begin_root() first");
    const auto found = roots_.find(root);
    // A window not drawn yet has no size to lay out in.
    if (found == roots_.end() || found->second.get() == root_) return false;
    Root& other = *found->second;
    const LayoutStyle style = layout_.style();
    suspended_.push_back({.root = root_,
                          .layout = std::move(layout_),
                          .areas = std::move(areas_),
                          .panel_stack = std::move(panel_stack_),
                          .open_containers = std::move(open_containers_),
                          .hit_targets = std::move(hit_targets_),
                          .hidden = hidden_});
    // Begun as begin_root() begins it, at the size it was last drawn at; its
    // own begin_root() goes on from what is drawn into it now.
    root_ = &other;
    if (other.begun != frame_) {
        other.begun = frame_;
        other.cursor = MouseCursor::Arrow;
        other.text_input = false;
        other.list.reset(other.draw, other.size, other.scale, &text_);
        other.base_hits.clear();
        other.areas_next.clear();
    }
    hit_targets_.assign(1, &other.base_hits);
    panel_stack_.clear();
    layout_ = Layout{};
    layout_.set_style(style);
    layout_.begin_area({.min = {0.0f, 0.0f}, .max = other.size});
    areas_.clear();
    open_containers_.clear();
    hidden_ = 0;
    return true;
}

void Context::end_other_root() {
    if (suspended_.empty()) {
        FTK_CORE_ERROR("UI: another window ended that was never drawn into");
        return;
    }
    if (!open_containers_.empty() || !areas_.empty()) {
        FTK_CORE_ERROR("UI: window {} drawn into with {} container(s) and {} panel(s) or region(s) left open",
                       root_->id, open_containers_.size(), areas_.size());
        while (!open_containers_.empty()) end_container();
        while (!areas_.empty()) areas_.back().panel ? end_panel() : end_region();
    }
    (void)layout_.end_area();
    Suspended back = std::move(suspended_.back());
    suspended_.pop_back();
    root_ = back.root;
    layout_ = std::move(back.layout);
    areas_ = std::move(back.areas);
    panel_stack_ = std::move(back.panel_stack);
    open_containers_ = std::move(back.open_containers);
    hit_targets_ = std::move(back.hit_targets);
    hidden_ = back.hidden;
}

void Context::open_root(const RootRequest& request) {
    std::erase(closed_roots_, request.root);
    output_.open.push_back(request);
}

void Context::close_root(RootId root) {
    output_.close.push_back(root);
}

void Context::closed_root(RootId root) {
    forget_root(root);
    if (!std::ranges::contains(closed_roots_, root)) closed_roots_.push_back(root);
}

bool Context::root_closed(RootId root) const {
    return std::ranges::contains(closed_roots_, root);
}

DrawList& Context::draw_list() {
    assert(root_ != nullptr && "UI: drawn outside a window; call begin_root() first");
    return root_->list;
}

Context::Panel& Context::panel(Id id) {
    for (const auto& panel : root_->panels) {
        if (panel->id == id) return *panel;
    }
    auto& added = root_->panels.emplace_back(std::make_unique<Panel>());
    added->id = id;
    added->order = root_->next_order++;
    return *added;
}

void Context::bring_to_front(Id id) {
    assert(root_ != nullptr && "UI: a panel brought forward outside a window; call begin_root() first");
    panel(id).order = root_->next_order++;
}

Id Context::current_panel() const {
    for (auto area = areas_.rbegin(); area != areas_.rend(); ++area) {
        if (area->panel) return area->id;
    }
    return 0;
}

void Context::begin_panel(std::string_view name, Rect rect, const PanelOptions& options) {
    begin_panel_id(ids_.id(name), rect, options);
}

bool Context::begin_outside(Id id, Rect rect, const PanelOptions& options, bool modal) {
    if (!popups_leave_window_ || !options.fit || modal) return false;
    const Layer layer = panel_stack_.empty() ? options.layer : std::max(options.layer, panel_stack_.back()->layer);
    if (layer != Layer::Popup && layer < Layer::Tooltip) return false;

    // How big it came to: in its own window when it was drawn there, else
    // in this one. Not measured yet, it is measured here first.
    const auto measured_in = [&](const Root& root) -> const Panel* {
        for (const auto& kept : root.panels) {
            if (kept->id == id && kept->measured) return kept.get();
        }
        return nullptr;
    };
    const Panel* measured = nullptr;
    if (const auto known = outside_.find(id); known != outside_.end()) {
        if (const auto there = roots_.find(known->second); there != roots_.end()) measured = measured_in(*there->second);
    }
    if (measured == nullptr) measured = measured_in(*root_);
    if (measured == nullptr) return false;
    const glm::vec2 size = glm::clamp(measured->content + 2.0f * options.padding + glm::vec2{0.0f, options.bar},
                                      options.min_size, options.max_size);
    const Rect wanted{.min = rect.min, .max = rect.min + size};
    if (wanted.min.x >= 0.0f && wanted.min.y >= 0.0f && wanted.max.x <= root_->size.x && wanted.max.y <= root_->size.y) {
        outside_.erase(id);
        return false;
    }

    // Its window closed by the platform (Wayland dismisses popups): the
    // popup closes with it, drawn inside this window this last frame.
    const RootId outside = hash_label("#OUTSIDE", id);
    if (root_closed(outside)) {
        std::erase(closed_roots_, outside);
        outside_.erase(id);
        close_popup(id);
        return false;
    }

    // Asked for, or asked to move and size as it is now; drawn there once
    // the program has opened it, inside this window until then.
    outside_[id] = outside;
    if (!std::ranges::contains(outside_now_, outside)) {
        open_root({.root = outside,
                   .kind = layer >= Layer::Tooltip ? RootKind::Tooltip : RootKind::Popup,
                   .size = size,
                   .near = root_->id,
                   .at = wanted.min});
        outside_now_.push_back(outside);
    }
    if (!begin_other_root(outside)) return false;
    PanelOptions there = options;
    there.fit = false;
    begin_panel_id(id, {.min = {0.0f, 0.0f}, .max = root_->size}, there);
    panel_stack_.back()->outside = true;
    return true;
}

void Context::begin_panel_id(Id id, Rect rect, const PanelOptions& options, bool modal) {
    assert(root_ != nullptr && "UI: a panel begun outside a window");
    // Past the window's edge, into an OS popup window of its own.
    if (begin_outside(id, rect, options, modal)) return;
    const Id parent_area = current_area();
    Panel& shown = panel(id);
    // Begun again this frame: drawn over what it holds, where it was.
    shown.again = shown.frame == frame_;
    if (shown.again) {
        rect = shown.rect;
    } else {
        // In its parent's layer at least, so it is drawn over its parent.
        shown.layer = panel_stack_.empty() ? options.layer : std::max(options.layer, panel_stack_.back()->layer);
        shown.hidden = false;
        shown.raise = options.raise;
    }
    if (options.fit && !shown.again) {
        if (shown.measured) {
            const glm::vec2 fits = shown.content + 2.0f * options.padding + glm::vec2{0.0f, options.bar};
            rect.max = rect.min + glm::clamp(fits, options.min_size, options.max_size);
        } else {
            // Not seen yet: as big as it is to be at least, while what it
            // holds is measured.
            rect.max = rect.min + options.min_size;
            shown.hidden = true;
            request_frame();
        }
        // Inside the window, moved back from its right and bottom edges.
        const glm::vec2 size = rect.size();
        const glm::vec2 over = glm::max(rect.max - root_->size, glm::vec2{0.0f});
        rect.min = glm::max(rect.min - over, glm::vec2{0.0f});
        rect.max = rect.min + size;
    }
    shown.rect = rect;
    shown.frame = frame_;
    panel_stack_.push_back(&shown);
    root_->list.push_target(shown.primitives, shown.again);
    root_->list.push_clip(rect.min, rect.max);
    if (!shown.again) shown.hits.clear();
    const Rect window{.min = {0.0f, 0.0f}, .max = root_->size};
    if (shown.layer >= Layer::Tooltip) {
        // Tooltips and drag previews take no pointers.
        no_hits_.clear();
        hit_targets_.push_back(&no_hits_);
    } else {
        hit_targets_.push_back(&shown.hits);
        // A dialog covers the whole window beneath it; any panel covers
        // its own rectangle, under its widgets.
        if (!shown.again) {
            const auto own = [&](Rect r) {
                shown.hits.push_back(
                    {.id = id, .rect = r, .panel = id, .area = id, .place = Place::Own, .sequence = hit_sequence_++});
            };
            if (modal) own(window);
            own(rect.intersect(window));
        }
    }
    // Its items' ids under its own, whatever it was begun inside.
    ids_.push_id(id);
    Scroll& scroll = scrolls_[id];
    if (options.scroll && !shown.again) scroll_area(id, scroll);
    const Rect inside{.min = rect.min + options.padding + glm::vec2{0.0f, options.bar}, .max = rect.max - options.padding};
    layout_.begin_area(inside, options.scroll ? scroll.offset : glm::vec2{0.0f, 0.0f});
    if (shown.again) layout_.set_cursor(shown.cursor);
    areas_.push_back({.id = id, .panel = true, .scrolls = options.scroll, .rect = rect, .visible = inside});
    root_->areas_next.push_back({.id = id, .parent = parent_area});
}

void Context::end_panel() {
    if (areas_.empty() || !areas_.back().panel) {
        FTK_CORE_ERROR("UI: a panel ended inside a region, or never began");
        return;
    }
    panel_stack_.back()->cursor = layout_.cursor();
    const glm::vec2 content = end_area();
    ids_.pop();
    root_->list.pop_clip();
    root_->list.pop_target();
    order_hits(*hit_targets_.back());
    hit_targets_.pop_back();
    // What it held, settled when the root ends: begun again, it holds what
    // it held the first time as well.
    Panel& ended = *panel_stack_.back();
    panel_stack_.pop_back();
    ended.content_next = ended.again ? glm::max(ended.content_next, content) : content;
    // Drawn in its own OS popup window: back to the window it was begun in.
    if (std::exchange(ended.outside, false)) end_other_root();
}

Rect Context::panel_rect() const {
    if (!panel_stack_.empty()) return panel_stack_.back()->rect;
    return {.min = {0.0f, 0.0f}, .max = root_ != nullptr ? root_->size : glm::vec2{0.0f}};
}

void Context::begin_region(std::string_view name, glm::vec2 size, const RegionOptions& options) {
    const Id id = ids_.id(name);
    const Id parent_area = current_area();
    const glm::vec2 room = layout_.space_left();
    const float height = size.y == FILL ? room.y : (size.y < 0.0f ? std::max(room.y + size.y, 0.0f) : size.y);
    const Rect rect = layout_.next({layout_.resolve_width(size.x), height});
    // Its own place, under its items: the pointer over it is over it, not
    // its panel, where it has nothing.
    const Rect visible = rect.intersect({.min = root_->list.clip_min(), .max = root_->list.clip_max()});
    if (!hidden() && !visible.empty()) {
        hit_targets_.back()->push_back({.id = id,
                                        .rect = visible,
                                        .panel = current_panel(),
                                        .area = id,
                                        .place = Place::Own,
                                        .sequence = hit_sequence_++});
    }
    root_->list.push_clip(rect.min, rect.max);
    ids_.push_id(id);
    Scroll& scroll = scrolls_[id];
    scroll_area(id, scroll);
    const Rect inside{.min = rect.min + options.padding,
                      .max = glm::max(rect.max - options.padding, rect.min + options.padding)};
    layout_.begin_area(inside, scroll.offset);
    areas_.push_back({.id = id, .panel = false, .scrolls = true, .rect = rect, .visible = inside});
    root_->areas_next.push_back({.id = id, .parent = parent_area, .wheel = options.wheel_scrolls});
}

Rect Context::area_rect() const {
    if (!areas_.empty()) return areas_.back().rect;
    return {.min = {0.0f, 0.0f}, .max = root_ != nullptr ? root_->size : glm::vec2{0.0f}};
}

Rect Context::area_inside() const {
    if (!areas_.empty()) return areas_.back().visible;
    return area_rect();
}

bool Context::area_under_pointer() const {
    if (root_ == nullptr) return false;
    const Id area = current_area();
    return std::ranges::any_of(pointers_, [&](const Tracked& tracked) {
        const Pointer& p = tracked.pointer;
        return p.root == root_->id && p.present && p.can_hover && tracked.top != 0 && tracked.top_area == area;
    });
}

bool Context::area_or_inner_under_pointer() const {
    if (root_ == nullptr) return false;
    const Id area = current_area();
    return std::ranges::any_of(pointers_, [&](const Tracked& tracked) {
        const Pointer& p = tracked.pointer;
        if (p.root != root_->id || !p.present || !p.can_hover || tracked.top == 0) return false;
        // Up from the area it is over, through the areas it is inside, as
        // last frame had them.
        for (Id over = tracked.top_area; over != 0;) {
            if (over == area) return true;
            const auto link = std::ranges::find(root_->areas, over, &AreaLink::id);
            if (link == root_->areas.end()) break;
            over = link->parent;
        }
        return false;
    });
}

bool Context::pointer_over(Rect rect) const {
    if (root_ == nullptr || hidden()) return false;
    const Id area = current_area();
    return std::ranges::any_of(pointers_, [&](const Tracked& tracked) {
        const Pointer& p = tracked.pointer;
        return p.root == root_->id && p.present && p.can_hover && tracked.top_own && tracked.top_area == area &&
               rect.contains(p.now.position);
    });
}

void Context::end_region() {
    if (areas_.empty() || areas_.back().panel) {
        FTK_CORE_ERROR("UI: a region ended inside a panel of its own, or never began");
        return;
    }
    end_area();
    ids_.pop();
    root_->list.pop_clip();
}

void Context::scroll_area(Id id, Scroll& scroll) {
    scroll.frame = frame_;
    const glm::vec2 room = glm::max(scroll.content - scroll.visible, glm::vec2{0.0f});
    if (id == wheel_area_) {
        // Turned up, what it holds comes down; turned right, it goes left.
        const glm::vec2 turn{wheel_.x, -wheel_.y};
        scroll.target = glm::clamp(scroll.target + turn * pointer_style_.wheel_step, glm::vec2{0.0f}, room);
        wheel_area_ = 0;
    }
    // Smooth: most of the way there each frame, then the rest at once.
    if (scroll.offset != scroll.target) {
        const float step = std::min(1.0f, delta_ * 18.0f);
        scroll.offset += (scroll.target - scroll.offset) * step;
        if (glm::all(glm::lessThan(glm::abs(scroll.target - scroll.offset), glm::vec2{0.5f}))) {
            scroll.offset = scroll.target;
        } else {
            request_frame();
        }
    }
}

glm::vec2 Context::end_area() {
    const Area area = areas_.back();
    areas_.pop_back();
    const glm::vec2 content = layout_.end_area();
    if (!area.scrolls) return content;
    Scroll& scroll = scrolls_[area.id];
    scroll.content = content;
    scroll.visible = area.visible.size();
    // What no longer reaches as far scrolls back, seen next frame.
    const glm::vec2 room = glm::max(content - scroll.visible, glm::vec2{0.0f});
    const glm::vec2 kept = glm::clamp(scroll.offset, glm::vec2{0.0f}, room);
    if (kept != scroll.offset) request_frame();
    scroll.offset = kept;
    scroll.target = glm::clamp(scroll.target, glm::vec2{0.0f}, room);
    for (AreaLink& link : root_->areas_next) {
        if (link.id != area.id) continue;
        link.room = room;
        link.offset = scroll.target;
    }
    return content;
}

glm::vec2 Context::scroll() const {
    for (auto area = areas_.rbegin(); area != areas_.rend(); ++area) {
        if (area->scrolls) return scrolls_.at(area->id).offset;
    }
    return {0.0f, 0.0f};
}

glm::vec2 Context::scroll_max() const {
    for (auto area = areas_.rbegin(); area != areas_.rend(); ++area) {
        if (!area->scrolls) continue;
        const Scroll& scroll = scrolls_.at(area->id);
        return glm::max(scroll.content - scroll.visible, glm::vec2{0.0f});
    }
    return {0.0f, 0.0f};
}

void Context::set_scroll(glm::vec2 offset) {
    for (auto area = areas_.rbegin(); area != areas_.rend(); ++area) {
        if (!area->scrolls) continue;
        Scroll& scroll = scrolls_.at(area->id);
        // Items already placed this frame stay; the next frame shows it.
        scroll.offset = glm::clamp(offset, glm::vec2{0.0f}, glm::max(scroll.content - scroll.visible, glm::vec2{0.0f}));
        scroll.target = scroll.offset;
        request_frame();
        return;
    }
}

void Context::begin_container(std::string_view name, const ContainerOptions& options) {
    const Id id = ids_.id(name);
    const auto kept = containers_.find(id);
    const bool known = kept != containers_.end();
    const bool hide = !options.wrap && !known;
    if (hide) {
        ++hidden_;
        request_frame();
    }
    open_containers_.push_back({.id = id, .measured = !options.wrap, .hidden = hide, .mark = root_->list.mark()});
    layout_.begin_container(options, known ? &kept->second.memory : nullptr);
}

void Context::end_container() {
    if (open_containers_.empty()) {
        FTK_CORE_ERROR("UI: a container ended that never began");
        return;
    }
    const OpenContainer open = open_containers_.back();
    open_containers_.pop_back();
    ContainerMemory measured;
    (void)layout_.end_container(measured);
    KeptContainer& kept = containers_[open.id];
    // Placed by measurements that have changed: the next frame places them
    // by these.
    if (open.measured && !open.hidden && kept.memory != measured) request_frame();
    kept.memory = measured;
    kept.frame = frame_;
    if (open.hidden) {
        root_->list.truncate(open.mark);
        --hidden_;
    }
}

const FrameOutput& Context::end_frame() {
    if (!suspended_.empty()) {
        FTK_CORE_ERROR("UI: the frame ended drawing into window {}", root_->id);
        while (!suspended_.empty()) end_other_root();
    }
    // The OS popup windows of popups not drawn this frame close.
    for (const RootId was : outside_last_) {
        if (std::ranges::contains(outside_now_, was)) continue;
        close_root(was);
        std::erase_if(outside_, [&](const auto& kept) { return kept.second == was; });
    }
    outside_last_ = std::exchange(outside_now_, {});
    if (root_ != nullptr) {
        FTK_CORE_ERROR("UI: the frame ended inside window {}", root_->id);
        end_root();
    }
    settle_layers();
    navigate();
    finish_semantics();
    in_frame_ = false;
    // A deadline already reached is no reason to sleep.
    if (next_now_ || (next_at_ && *next_at_ <= time_)) {
        output_.next.now = true;
    } else {
        output_.next.at = next_at_;
    }
    return output_;
}

void Context::request_frame_at(double time) {
    next_at_ = next_at_ ? std::min(*next_at_, time) : time;
}

Context& current() {
    assert(current_context != nullptr && "UI: no context set; call core::set_current() first");
    return *current_context;
}

void set_current(Context* context) {
    current_context = context;
}

} // namespace ftk::ui::core
