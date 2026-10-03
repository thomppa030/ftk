// The UI core's layers (decision 22 of the UI core plan): popups closed by
// a press outside them or Escape, dialogs that block what is beneath them,
// tooltips once the pointer rests or a finger long-presses, and drag and
// drop with typed payloads judged while they hover. Each is a panel in its
// layer of the window, drawn over the layers beneath it.

#include "ftk/ui/core/context.hpp"

#include <algorithm>
#include <cstring>

namespace ftk::ui::core {

namespace {

// Where a tooltip and a drag preview sit from the pointer.
constexpr glm::vec2 TOOLTIP_OFFSET{16.0f, 18.0f};
constexpr glm::vec2 PREVIEW_OFFSET{12.0f, 12.0f};

} // namespace

void Context::open_popup(Id id) {
    open_popup(id, last_pointer_position_);
}

void Context::open_popup(Id id, glm::vec2 at) {
    request_frame();
    if (popup_open(id)) return;
    // Opened between frames, it is the first window's that draws it.
    popups_.push_back({.id = id, .root = root_ != nullptr ? root_->id : 0, .at = at, .opened = frame_});
}

bool Context::popup_open(Id id) const {
    return std::ranges::find(popups_, id, &OpenPopup::id) != popups_.end();
}

void Context::close_popup(Id id) {
    const auto open = std::ranges::find(popups_, id, &OpenPopup::id);
    if (open != popups_.end()) close_popups_from(static_cast<std::size_t>(open - popups_.begin()));
}

void Context::close_popups_from(std::size_t index) {
    if (index >= popups_.size()) return;
    popups_.erase(popups_.begin() + static_cast<std::ptrdiff_t>(index), popups_.end());
    request_frame();
}

bool Context::begin_popup(Id id, PanelOptions options) {
    return begin_open_popup(id, options, false);
}

bool Context::begin_modal_popup(Id id, PanelOptions options) {
    // Over the whole window, so a press outside it lands in it and closes
    // only what was opened from it.
    options.layer = Layer::Modal;
    return begin_open_popup(id, options, true);
}

bool Context::begin_open_popup(Id id, const PanelOptions& options, bool modal) {
    const auto open = std::ranges::find(popups_, id, &OpenPopup::id);
    if (open == popups_.end() || root_ == nullptr || (open->root != 0 && open->root != root_->id)) return false;
    open->root = root_->id;
    open->drawn = frame_;
    begin_panel_id(id, {.min = open->at, .max = open->at}, options, modal);
    return true;
}

void Context::begin_modal(std::string_view name, Rect rect, PanelOptions options) {
    options.layer = Layer::Modal;
    begin_panel_id(ids_.id(name), rect, options, true);
}

bool Context::tooltip_due() {
    const Interaction& in = last_interaction_;
    // A finger long-pressed it: shown while the finger stays.
    if (in.held_long) return true;
    if (!(in.hovered || last_rests_disabled_) || in.held) return false;
    // Rested on since the frame that found it, or from this one.
    const double since = hover_id_ == last_widget_ ? hover_since_ : time_;
    const double due = since + pointer_style_.tooltip_delay;
    if (hover_id_ == last_widget_ && time_ >= due) return true;
    request_frame_at(due);
    return false;
}

void Context::begin_tooltip() {
    const glm::vec2 at = last_pointer_position_ + TOOLTIP_OFFSET;
    begin_panel_id(hash_label("##tooltip", 0), {.min = at, .max = at},
                   {.scroll = false, .layer = Layer::Tooltip, .fit = true});
}

const Context::Tracked* Context::drag_pointer() const {
    for (const Tracked& tracked : pointers_) {
        if (tracked.pointer.type == drag_.pointer_type && tracked.pointer.id == drag_.pointer) return &tracked;
    }
    return nullptr;
}

bool Context::drag_source() {
    const Interaction& in = last_interaction_;
    if (!in.held || !in.dragging) return false;
    if (drag_.active && drag_.source != last_widget_) return false;
    if (!drag_.active) {
        drag_ = Drag{};
        drag_.active = true;
        drag_.source = last_widget_;
        drag_.pointer_type = in.pointer_type;
        drag_.pointer = in.pointer;
    }
    drag_.seen = true;
    return true;
}

void Context::carry(std::string_view type, const void* data, std::size_t size) {
    if (!drag_.active) return;
    drag_.type = type;
    drag_.data.resize(size);
    if (size > 0) std::memcpy(drag_.data.data(), data, size);
}

std::optional<Drop> Context::drop_target(std::string_view type) {
    if (!drag_.active || drag_.type != type || drag_.source == last_widget_) return std::nullopt;
    const Tracked* by = drag_pointer();
    if (by == nullptr || by->top != last_widget_ || !last_interaction_.under) return std::nullopt;
    return Drop{.payload = {.data = drag_.data.data(), .size = drag_.data.size()}, .delivered = by->pointer.released != 0};
}

std::optional<Drop> Context::drop_target(Rect rect, std::string_view type) {
    if (!drag_.active || drag_.type != type || root_ == nullptr) return std::nullopt;
    const Tracked* by = drag_pointer();
    if (by == nullptr || by->pointer.root != root_->id) return std::nullopt;
    const Rect visible = rect.intersect({.min = root_->list.clip_min(), .max = root_->list.clip_max()});
    // Over it, and not over another panel lying on it.
    if (!visible.contains(by->pointer.now.position) || by->top_panel != current_panel()) return std::nullopt;
    return Drop{.payload = {.data = drag_.data.data(), .size = drag_.data.size()}, .delivered = by->pointer.released != 0};
}

void Context::begin_drag_preview() {
    const Tracked* by = drag_pointer();
    const glm::vec2 at = (by != nullptr ? by->pointer.now.position : last_pointer_position_) + PREVIEW_OFFSET;
    begin_panel_id(hash_label("##drag preview", 0), {.min = at, .max = at},
                   {.scroll = false, .layer = Layer::DragPreview, .fit = true});
}

void Context::settle_layers() {
    // A popup its opener stopped drawing closes, with those opened from it;
    // one opened this frame is drawn from the next.
    for (std::size_t i = 0; i < popups_.size(); ++i) {
        if (popups_[i].drawn != frame_ && popups_[i].opened != frame_) {
            close_popups_from(i);
            break;
        }
    }
    if (!popups_.empty() && key_pressed(Key::Escape) && !key_claimed(Key::Escape)) close_popups_from(popups_.size() - 1);

    // The drag ends when its pointer is let go or gone, or its source stops
    // being drawn; it is forgotten as the next frame begins.
    if (drag_.active) {
        const Tracked* by = drag_pointer();
        if (by == nullptr || !drag_.seen || by->pointer.released != 0 || by->pointer.cancelled) {
            drag_.ended = true;
            request_frame();
        }
    }

    // The widget the mouse rests on, and since when, for tooltips.
    if (hover_next_ != hover_id_) {
        hover_id_ = hover_next_;
        hover_since_ = time_;
    }
}

} // namespace ftk::ui::core
