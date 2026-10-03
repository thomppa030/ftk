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
    next_now_ = false;
    next_at_.reset();
    output_.roots.clear();
    output_.next = {};
    last_widget_ = 0;
    last_interaction_ = {};
    any_hovered_ = false;
    hover_next_ = 0;
    drag_.seen = false;
    disabled_.clear();
    text_.begin_frame();
    take_input();
    take_keys();
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
    root_->cursor = MouseCursor::Arrow;
    root_->text_input = false;
    root_->list.reset(root_->draw, size, scale, &text_);
    root_->base_hits.clear();
    root_->areas_next.clear();
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

void Context::forget_root(RootId root) {
    if (root_ != nullptr && root_->id == root) {
        FTK_CORE_ERROR("UI: window {} forgotten while it is drawn", root);
        return;
    }
    roots_.erase(root);
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

Id Context::current_panel() const {
    for (auto area = areas_.rbegin(); area != areas_.rend(); ++area) {
        if (area->panel) return area->id;
    }
    return 0;
}

void Context::begin_panel(std::string_view name, Rect rect, const PanelOptions& options) {
    begin_panel_id(ids_.id(name), rect, options);
}

void Context::begin_panel_id(Id id, Rect rect, const PanelOptions& options, bool modal) {
    assert(root_ != nullptr && "UI: a panel begun outside a window");
    const Id parent_area = current_area();
    Panel& shown = panel(id);
    // In its parent's layer at least, so it is drawn over its parent.
    shown.layer = panel_stack_.empty() ? options.layer : std::max(options.layer, panel_stack_.back()->layer);
    shown.hidden = false;
    if (options.fit) {
        if (shown.measured) {
            rect.max = rect.min + shown.content + 2.0f * options.padding;
        } else {
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
    root_->list.push_target(shown.primitives);
    root_->list.push_clip(rect.min, rect.max);
    shown.hits.clear();
    const Rect window{.min = {0.0f, 0.0f}, .max = root_->size};
    if (shown.layer >= Layer::Tooltip) {
        // Tooltips and drag previews take no pointers.
        no_hits_.clear();
        hit_targets_.push_back(&no_hits_);
    } else {
        hit_targets_.push_back(&shown.hits);
        // A dialog covers the whole window beneath it; any panel covers
        // its own rectangle, under its widgets.
        if (modal) shown.hits.push_back({.id = id, .rect = window, .panel = id, .area = id});
        shown.hits.push_back({.id = id, .rect = rect.intersect(window), .panel = id, .area = id});
    }
    // Its items' ids under its own, whatever it was begun inside.
    ids_.push_id(id);
    Scroll& scroll = scrolls_[id];
    if (options.scroll) scroll_area(id, scroll);
    const Rect inside{.min = rect.min + options.padding, .max = rect.max - options.padding};
    layout_.begin_area(inside, options.scroll ? scroll.offset : glm::vec2{0.0f, 0.0f});
    areas_.push_back({.id = id, .panel = true, .scrolls = options.scroll, .visible = inside});
    root_->areas_next.push_back({.id = id, .parent = parent_area});
}

void Context::end_panel() {
    if (areas_.empty() || !areas_.back().panel) {
        FTK_CORE_ERROR("UI: a panel ended inside a region, or never began");
        return;
    }
    const glm::vec2 content = end_area();
    ids_.pop();
    root_->list.pop_clip();
    root_->list.pop_target();
    hit_targets_.pop_back();
    // What it held, for fitting it next frame; a change is drawn then.
    Panel& ended = *panel_stack_.back();
    panel_stack_.pop_back();
    if (ended.measured && ended.content != content) request_frame();
    ended.content = content;
    ended.measured = true;
}

Rect Context::panel_rect() const {
    if (!panel_stack_.empty()) return panel_stack_.back()->rect;
    return {.min = {0.0f, 0.0f}, .max = root_ != nullptr ? root_->size : glm::vec2{0.0f}};
}

void Context::begin_region(std::string_view name, glm::vec2 size) {
    const Id id = ids_.id(name);
    const Id parent_area = current_area();
    const glm::vec2 room = layout_.space_left();
    const float height = size.y == FILL ? room.y : (size.y < 0.0f ? std::max(room.y + size.y, 0.0f) : size.y);
    const Rect rect = layout_.next({layout_.resolve_width(size.x), height});
    root_->list.push_clip(rect.min, rect.max);
    ids_.push_id(id);
    Scroll& scroll = scrolls_[id];
    scroll_area(id, scroll);
    layout_.begin_area(rect, scroll.offset);
    areas_.push_back({.id = id, .panel = false, .scrolls = true, .visible = rect});
    root_->areas_next.push_back({.id = id, .parent = parent_area});
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
    if (root_ != nullptr) {
        FTK_CORE_ERROR("UI: the frame ended inside window {}", root_->id);
        end_root();
    }
    settle_layers();
    navigate();
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
