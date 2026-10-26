// The UI core's pointers (decision 8 of the UI core plan): the events a
// frame takes, what is under each pointer, and what the widgets make of
// them.
//
// Every widget leaves where it took the pointers in this frame's hits,
// bottom to top: a panel's over the root's, a later widget's over an
// earlier one's. The next frame's hover and presses go to whatever was on
// top there, so what is drawn over another takes the pointer without
// anything said. When the hits move under a still pointer, the frame asks
// for another, which finds the new top.

#include "ftk/ui/core/context.hpp"

#include "ftk/base/log.hpp"

#include <glm/geometric.hpp>

#include <algorithm>

namespace ftk::ui::core {

namespace {

// The first of `buttons`, left before right before middle.
MouseButton first_button(uint8_t buttons) {
    if ((buttons & button_bit(MouseButton::Left)) != 0) return MouseButton::Left;
    if ((buttons & button_bit(MouseButton::Right)) != 0) return MouseButton::Right;
    return MouseButton::Middle;
}

std::size_t index(MouseButton button) {
    return static_cast<std::size_t>(button);
}

} // namespace

const Context::Hit* Context::top_hit(const std::vector<Hit>& hits, glm::vec2 at) {
    for (auto hit = hits.rbegin(); hit != hits.rend(); ++hit) {
        if (hit->rect.contains(at)) return &*hit;
    }
    return nullptr;
}

void Context::add_hit(Id id, Rect rect, Place place) {
    hit_targets_.back()->push_back(
        {.id = id, .rect = rect, .panel = current_panel(), .area = current_area(), .place = place, .sequence = hit_sequence_++});
}

void Context::order_hits(std::vector<Hit>& hits) {
    const bool ordered = std::ranges::all_of(hits, [](const Hit& hit) {
        return hit.place == Place::Own || hit.place == Place::Over;
    });
    if (ordered) return;
    std::ranges::sort(hits, [](const Hit& a, const Hit& b) {
        if (a.place != b.place) return a.place < b.place;
        // Beneath what came before it: the first placed is on top.
        if (a.place == Place::Beneath) return a.sequence > b.sequence;
        return a.sequence < b.sequence;
    });
}

Context::Tracked& Context::track(PointerType type, PointerId id) {
    for (Tracked& tracked : pointers_) {
        if (tracked.pointer.type == type && tracked.pointer.id == id) return tracked;
    }
    Tracked& added = pointers_.emplace_back();
    added.pointer.type = type;
    added.pointer.id = id;
    added.pointer.can_hover = type != PointerType::Touch;
    return added;
}

const Pointer* Context::find_pointer(PointerType type, PointerId id) const {
    for (const Tracked& tracked : pointers_) {
        if (tracked.pointer.type == type && tracked.pointer.id == id) return &tracked.pointer;
    }
    return nullptr;
}

void Context::apply(const PointerEvent& event) {
    Tracked& tracked = track(event.type, event.id);
    Pointer& p = tracked.pointer;
    p.root = event.root;
    p.eraser = event.eraser;
    const auto record = [&] {
        // A pointer that has just come moved from where it came in.
        if (!p.present && p.samples.empty()) p.previous = event.sample.position;
        p.now = event.sample;
        p.samples.push_back(event.sample);
    };
    if (event.kind != PointerEvent::Kind::Leave) last_pointer_position_ = event.sample.position;
    const uint8_t bit = button_bit(event.button);
    const std::size_t b = index(event.button);
    switch (event.kind) {
    case PointerEvent::Kind::Move:
        record();
        // A finger is somewhere only while it is down.
        if (p.type != PointerType::Touch) p.present = true;
        break;
    case PointerEvent::Kind::Down: {
        record();
        p.present = true;
        p.buttons |= bit;
        p.pressed |= bit;
        const bool again = p.click_count[b] > 0 &&
                           event.sample.time - p.last_press_time[b] <= pointer_style_.double_click_time &&
                           glm::distance(event.sample.position, p.press_position[b]) <= pointer_style_.double_click_distance;
        p.click_count[b] = again ? p.click_count[b] + 1 : 1;
        p.last_press_time[b] = event.sample.time;
        p.press_position[b] = event.sample.position;
        input_mode_ = p.type;
        break;
    }
    case PointerEvent::Kind::Up:
        record();
        p.buttons &= static_cast<uint8_t>(~bit);
        p.released |= bit;
        p.release_position = event.sample.position;
        if (p.type == PointerType::Touch) tracked.ended = true;
        break;
    case PointerEvent::Kind::Cancel:
        record();
        p.cancelled = true;
        p.released |= p.buttons;
        p.buttons = 0;
        if (p.type == PointerType::Touch) tracked.ended = true;
        break;
    case PointerEvent::Kind::Leave:
        p.present = false;
        break;
    case PointerEvent::Kind::Wheel:
        p.now.position = event.sample.position;
        p.present = true;
        p.wheel += event.wheel;
        break;
    }
}

void Context::take_input() {
    // A drag that ended last frame is over.
    if (drag_.ended) drag_ = Drag{};
    // What the last frame let go of is free again, and so is what was held
    // by a widget that wasn't drawn, or by a palm; lifted fingers are
    // forgotten.
    for (Tracked& tracked : pointers_) {
        Pointer& p = tracked.pointer;
        if (tracked.holder != 0) {
            const bool let_go = (p.released & button_bit(tracked.hold_button)) != 0 || p.cancelled ||
                                !tracked.holder_seen || tracked.rejected;
            if (let_go) {
                tracked.holder = 0;
                tracked.dragging = false;
                tracked.long_pressed = false;
            }
        }
        tracked.holder_seen = false;
        tracked.press_top = 0;
        tracked.frame_start = p.now.position;
        p.previous = p.now.position;
        p.pressed = 0;
        p.released = 0;
        p.cancelled = false;
        p.samples.clear();
        p.wheel = {0.0f, 0.0f};
    }
    std::erase_if(pointers_, [](const Tracked& tracked) { return tracked.ended; });
    // A pointer let go is let go this frame where it was let go, in the
    // window it was held in, and what follows waits for the next frame, in
    // order: past a window's edge the platform moves it back to the edge as
    // it lets go of the mouse, and it may come into another window (a tab
    // dragged onto another window's row) before the frame.
    std::vector<PointerEvent> waiting;
    for (const PointerEvent& event : events_) {
        const bool after_one_waiting = std::ranges::any_of(waiting, [&](const PointerEvent& earlier) {
            return earlier.type == event.type && earlier.id == event.id;
        });
        const Pointer* p = find_pointer(event.type, event.id);
        const bool let_go = p != nullptr && (p->released != 0 || p->cancelled);
        if (after_one_waiting || let_go) {
            waiting.push_back(event);
            continue;
        }
        apply(event);
    }
    events_ = std::move(waiting);
    if (!events_.empty()) request_frame();

    // What is on top under each pointer, and where each press was, by the
    // last frame's hits.
    wheel_area_ = 0;
    wheel_ = {0.0f, 0.0f};
    any_press_ = false;
    for (auto& [id, root] : roots_) root->pen_near = false;
    for (Tracked& tracked : pointers_) {
        const Pointer& p = tracked.pointer;
        any_press_ = any_press_ || p.pressed != 0;
        const auto root = roots_.find(p.root);
        if (root == roots_.end()) {
            tracked.top = 0;
            continue;
        }
        Root& kept = *root->second;
        if (p.type == PointerType::Pen && p.present) kept.pen_near = true;
        const Hit* top = p.present ? top_hit(kept.hits, p.now.position) : nullptr;
        tracked.top = top != nullptr ? top->id : 0;
        tracked.top_panel = top != nullptr ? top->panel : 0;
        tracked.top_area = top != nullptr ? top->area : 0;
        tracked.top_own = top != nullptr && top->place == Place::Own;
        if (p.pressed != 0) {
            const glm::vec2 at = p.press_position[index(first_button(p.pressed))];
            // Gestures measure a finger that came down this frame from there.
            if (tracked.holder == 0) tracked.frame_start = at;
            const Hit* pressed = top_hit(kept.hits, at);
            tracked.press_top = pressed != nullptr ? pressed->id : 0;
            const Id panel_pressed = pressed != nullptr ? pressed->panel : 0;
            // A press brings the panel it lands in to the front, unless it
            // stays behind.
            if (panel_pressed != 0) {
                for (const auto& panel : kept.panels) {
                    if (panel->id == panel_pressed && panel->raise) panel->order = kept.next_order++;
                }
            }
            // It closes the popups it isn't in: those over the one it is in,
            // or every one of the window when it is in none.
            const auto in_popup = std::ranges::find(popups_, panel_pressed, &OpenPopup::id);
            if (in_popup != popups_.end()) {
                close_popups_from(static_cast<std::size_t>(in_popup - popups_.begin()) + 1);
            } else {
                std::erase_if(popups_, [&](const OpenPopup& open) { return open.root == p.root; });
            }
        }
        // The wheel turns the innermost area under the mouse that can still
        // scroll that way, and takes the wheel.
        if (p.wheel != glm::vec2{0.0f} && top != nullptr) {
            for (Id area = top->area; area != 0;) {
                const auto link = std::ranges::find(kept.areas, area, &AreaLink::id);
                if (link == kept.areas.end()) break;
                const bool down = p.wheel.y < 0.0f && link->offset.y < link->room.y;
                const bool up = p.wheel.y > 0.0f && link->offset.y > 0.0f;
                const bool right = p.wheel.x > 0.0f && link->offset.x < link->room.x;
                const bool left = p.wheel.x < 0.0f && link->offset.x > 0.0f;
                if (link->wheel && (down || up || right || left)) {
                    wheel_area_ = area;
                    wheel_ = p.wheel;
                    break;
                }
                area = link->parent;
            }
        }
    }
}

Interaction Context::interact(Id id, Rect rect, Sense sense) {
    last_widget_ = id;
    last_interaction_ = {};
    last_rests_disabled_ = false;
    if (root_ == nullptr) return {};
    const Rect visible = rect.intersect({.min = root_->list.clip_min(), .max = root_->list.clip_max()});
    if (hidden()) return {};
    if (visible.empty()) {
        // Out of sight a widget is pressed and hovered no more, but one that
        // holds a pointer keeps it: a number dragged as its panel scrolls
        // goes on being dragged.
        const bool holds = std::ranges::any_of(pointers_, [&](const Tracked& tracked) {
            return tracked.pointer.root == root_->id && tracked.holder == id;
        });
        if (!holds) return {};
    } else {
        add_hit(id, visible, !sense.beneath ? Place::Over : (sense.overlap ? Place::Overlapped : Place::Beneath));
    }
    if (disabled()) {
        // It takes nothing, but a hovering pointer resting on it is told to
        // its tooltip.
        last_rests_disabled_ = std::ranges::any_of(pointers_, [&](const Tracked& tracked) {
            const Pointer& p = tracked.pointer;
            return p.root == root_->id && p.present && p.can_hover && tracked.holder == 0 && tracked.top == id &&
                   visible.contains(p.now.position);
        });
        if (last_rests_disabled_) hover_next_ = id;
        return {};
    }

    const uint8_t takes = static_cast<uint8_t>((sense.left ? button_bit(MouseButton::Left) : 0) |
                                               (sense.right ? button_bit(MouseButton::Right) : 0) |
                                               (sense.middle ? button_bit(MouseButton::Middle) : 0));
    // A finger on a canvas while a pen is near is a palm.
    const bool palms = sense.canvas && root_->pen_near;
    Interaction in;
    const auto describe = [&](const Tracked& tracked, MouseButton button) {
        const Pointer& p = tracked.pointer;
        in.pointer_type = p.type;
        in.pointer = p.id;
        in.button = button;
        in.position = p.now.position;
        in.press_position = p.press_position[index(button)];
        in.pressure = p.now.pressure;
    };
    for (Tracked& tracked : pointers_) {
        Pointer& p = tracked.pointer;
        if (p.root != root_->id) continue;
        const bool over = p.present && tracked.top == id && visible.contains(p.now.position);
        in.under = in.under || over;
        const bool touch = p.type == PointerType::Touch;

        // A press where this was on top: it holds the pointer from now on.
        const uint8_t pressed = p.pressed & takes;
        if (tracked.holder == 0 && !tracked.rejected && pressed != 0 && tracked.press_top == id &&
            visible.contains(p.press_position[index(first_button(pressed))])) {
            if (touch && palms) {
                tracked.rejected = true;
                continue;
            }
            tracked.holder = id;
            tracked.hold_button = first_button(pressed);
            tracked.held_since = frame_;
            tracked.dragging = false;
            tracked.long_pressed = false;
            press_kept_keyboard_ = press_kept_keyboard_ || sense.keeps_keyboard;
        }
        if (tracked.holder == id) {
            tracked.holder_seen = true;
            const MouseButton button = tracked.hold_button;
            describe(tracked, button);
            in.held = true;
            // A palm found out while it holds the canvas: dropped.
            if (touch && palms) tracked.rejected = true;
            in.pressed = in.pressed || tracked.held_since == frame_;
            in.double_clicked = in.double_clicked || (tracked.held_since == frame_ && p.click_count[index(button)] == 2);
            const float threshold =
                touch ? pointer_style_.touch_drag_threshold : pointer_style_.drag_threshold;
            if (!tracked.dragging) {
                tracked.dragging = glm::distance(p.now.position, p.press_position[index(button)]) > threshold;
            }
            in.dragging = in.dragging || tracked.dragging;
            // A finger still for long enough, where a mouse or a pen would
            // hover: once, at a deadline rather than by drawing every frame
            // until then.
            const bool down = (p.buttons & button_bit(button)) != 0;
            if (touch && down && !tracked.dragging && !tracked.long_pressed && !tracked.rejected) {
                const double at = p.last_press_time[index(button)] + pointer_style_.long_press_time;
                if (time_ >= at) {
                    tracked.long_pressed = true;
                    in.long_pressed = true;
                } else {
                    request_frame_at(at);
                }
            }
            in.held_long = in.held_long || tracked.long_pressed;
            if (p.cancelled || tracked.rejected) {
                in.cancelled = true;
                in.released = true;
            } else if ((p.released & button_bit(button)) != 0) {
                in.released = true;
                in.clicked = in.clicked || (visible.contains(p.release_position) && !tracked.long_pressed);
            }
            in.hovered = in.hovered || (p.can_hover && over);
            continue;
        }
        // Held by another widget, the pointer hovers nothing else.
        if (tracked.holder == 0 && p.can_hover && over) {
            in.hovered = true;
            if (!in.held) describe(tracked, MouseButton::Left);
        }
    }

    // The fingers holding it, as one gesture.
    int touches = 0;
    const Tracked* fingers[2]{};
    bool all_lifted = true;
    bool moved = false;
    for (const Tracked& tracked : pointers_) {
        if (tracked.holder != id || tracked.pointer.type != PointerType::Touch || tracked.rejected) continue;
        if (touches < 2) fingers[touches] = &tracked;
        ++touches;
        all_lifted = all_lifted && (tracked.pointer.released != 0 || tracked.pointer.cancelled);
        moved = moved || tracked.dragging;
    }
    int most_touches = touches;
    if (touches > 0) {
        GestureState& gesture = gestures_[id];
        if (gesture.most_touches == 0) gesture.began = time_;
        in.gesture.touches = touches;
        in.gesture.began = touches >= 2 && gesture.touches < 2;
        gesture.touches = touches;
        gesture.most_touches = std::max(gesture.most_touches, touches);
        gesture.moved = gesture.moved || moved;
        gesture.frame = frame_;
        most_touches = gesture.most_touches;
        if (touches >= 2) {
            const glm::vec2 a = fingers[0]->pointer.now.position;
            const glm::vec2 b = fingers[1]->pointer.now.position;
            const glm::vec2 a0 = fingers[0]->frame_start;
            const glm::vec2 b0 = fingers[1]->frame_start;
            in.gesture.centre = 0.5f * (a + b);
            in.gesture.scale = glm::distance(a, b) / std::max(glm::distance(a0, b0), 1e-3f);
            in.gesture.pan = in.gesture.centre - 0.5f * (a0 + b0);
        }
        if (all_lifted) {
            const bool tap = gesture.most_touches >= 2 && !gesture.moved && time_ - gesture.began <= pointer_style_.tap_time;
            if (tap) in.gesture.taps = gesture.most_touches;
            gestures_.erase(id);
        }
    }
    const bool finger = in.pointer_type == PointerType::Touch;
    in.stroke = in.held && !in.cancelled && (!finger || (!sense.pen_only && most_touches < 2));

    any_hovered_ = any_hovered_ || in.hovered;
    if (in.hovered) hover_next_ = id;
    last_interaction_ = in;
    return in;
}

bool Context::any_held() const {
    return std::ranges::any_of(pointers_, [](const Tracked& tracked) { return tracked.holder != 0; });
}

void Context::pop_disabled() {
    if (disabled_.empty()) {
        FTK_CORE_ERROR("UI: a disabled scope popped that was never pushed");
        return;
    }
    disabled_.pop_back();
}

void Context::set_cursor_shape(MouseCursor cursor) {
    if (root_ != nullptr) root_->cursor = cursor;
}

} // namespace ftk::ui::core
