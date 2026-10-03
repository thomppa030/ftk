// The UI core's keyboard (decision 22 of the UI core plan): keys and text
// as a frame sees them, and which widget has the keyboard. Widgets that can
// have it say so as they are drawn, so Tab moves in the order they were
// drawn, and the arrows move inside a list, a tree or a menu. Moving
// happens as the frame ends, after shortcuts have had the chance to claim
// a key, and the next frame shows it.

#include "ftk/ui/core/context.hpp"

#include "ftk/base/log.hpp"

#include <algorithm>

namespace ftk::ui::core {

namespace {

std::size_t slot(Key key) {
    return static_cast<std::size_t>(key);
}

} // namespace

void Context::set_composition(std::string_view text, int cursor) {
    composition_next_ = std::pair{std::string(text), cursor};
}

void Context::take_keys() {
    Keyboard& k = keyboard_;
    k.pressed.fill(false);
    k.repeated.fill(false);
    k.released.fill(false);
    k.typed = std::move(typed_next_);
    typed_next_.clear();
    if (composition_next_) {
        k.composition = std::move(composition_next_->first);
        k.composition_cursor = composition_next_->second;
        composition_next_.reset();
    }
    for (const KeyEvent& event : key_events_) {
        k.modifiers = event.modifiers;
        k.root = event.root;
        if (event.kind == KeyEvent::Kind::Modifiers) continue;
        const std::size_t key = slot(event.key);
        if (event.kind == KeyEvent::Kind::Down) {
            if (!event.repeat) k.pressed[key] = true;
            k.repeated[key] = true;
            k.down[key] = true;
        } else {
            k.released[key] = true;
            k.down[key] = false;
        }
    }
    key_events_.clear();
    // A claim lapses once its key is up.
    for (std::size_t key = 0; key < KEY_COUNT; ++key) {
        if (!k.down[key] && !k.pressed[key]) k.claims[key] = 0;
    }
}

bool Context::key_pressed(Key key) const {
    return !keyboard_.ignored && keyboard_.pressed[slot(key)];
}

bool Context::key_repeated(Key key) const {
    return !keyboard_.ignored && keyboard_.repeated[slot(key)];
}

bool Context::key_down(Key key) const {
    return !keyboard_.ignored && keyboard_.down[slot(key)];
}

bool Context::key_released(Key key) const {
    return !keyboard_.ignored && keyboard_.released[slot(key)];
}

bool Context::any_key_pressed() const {
    return !keyboard_.ignored && std::ranges::any_of(keyboard_.pressed, [](bool pressed) { return pressed; });
}

std::string_view Context::typed() const {
    return keyboard_.ignored ? std::string_view{} : std::string_view{keyboard_.typed};
}

void Context::set_clipboard(std::string_view text) {
    if (clipboard_.set) {
        clipboard_.set(text);
    } else {
        own_clipboard_ = text;
    }
}

std::string Context::clipboard() const {
    return clipboard_.get ? clipboard_.get() : own_clipboard_;
}

void Context::claim_key(Key key, Id owner) {
    keyboard_.claims[slot(key)] = owner;
}

bool Context::key_claimed(Key key) const {
    return keyboard_.claims[slot(key)] != 0;
}

Focus Context::focusable(Id id, const FocusOptions& options) {
    const Id scope = focus_scopes_.empty() ? 0 : focus_scopes_.back().first;
    const Axis axis = focus_scopes_.empty() ? Axis::Column : focus_scopes_.back().second;
    focus_order_.push_back({.id = id, .scope = scope, .axis = axis, .root = root_ != nullptr ? root_->id : 0});
    // Pressed just now: it has the keyboard from here.
    if (last_widget_ == id && last_interaction_.pressed) {
        focused_ = id;
        focus_given_ = true;
    }
    Focus focus;
    focus.focused = focused_ == id;
    const auto told = std::ranges::find(told_focused_, id);
    if (focus.focused) {
        focus_seen_ = true;
        focused_text_ = options.text;
        focus.gained = told == told_focused_.end();
        if (focus.gained) told_focused_.push_back(id);
    } else if (told != told_focused_.end()) {
        focus.lost = true;
        told_focused_.erase(told);
    }
    if (focus.focused && !options.text) {
        const bool enter = key_pressed(Key::Enter) && !key_claimed(Key::Enter);
        const bool space = key_pressed(Key::Space) && !key_claimed(Key::Space);
        focus.activated = enter || space;
    }
    return focus;
}

void Context::set_focus(Id id) {
    if (focused_ == id) return;
    focused_ = id;
    focused_text_ = false;
    // Given now, it counts as drawn and given, so the frame's end keeps it
    // whatever was pressed.
    focus_seen_ = id != 0;
    focus_given_ = true;
    request_frame();
}

void Context::push_focus_scope(Id id, Axis axis) {
    focus_scopes_.emplace_back(id, axis);
}

void Context::pop_focus_scope() {
    if (focus_scopes_.empty()) {
        FTK_CORE_ERROR("UI: a focus scope popped that was never pushed");
        return;
    }
    focus_scopes_.pop_back();
}

void Context::set_text_input_area(Rect area) {
    if (root_ == nullptr) return;
    root_->text_input = true;
    root_->text_area = area;
}

void Context::navigate() {
    const Id before = focused_;
    // Its widget wasn't drawn; or a press landed somewhere that didn't take
    // the keyboard.
    if (focused_ != 0 && (!focus_seen_ || (any_press_ && !focus_given_))) focused_ = 0;

    const auto usable = [&](Key key) { return key_repeated(key) && !key_claimed(key); };
    const auto at = std::ranges::find(focus_order_, focused_, &Focusable::id);
    const bool has = focused_ != 0 && at != focus_order_.end();

    // Tab and Shift+Tab: through every widget of the window, round.
    if (usable(Key::Tab)) {
        const RootId root = has ? at->root : keyboard_.root;
        std::vector<Id> order;
        for (const Focusable& f : focus_order_) {
            if (f.root == root) order.push_back(f.id);
        }
        if (!order.empty()) {
            const auto where = std::ranges::find(order, focused_);
            const auto count = static_cast<std::ptrdiff_t>(order.size());
            std::ptrdiff_t next = 0;
            if (where == order.end()) {
                next = keyboard_.modifiers.shift ? count - 1 : 0;
            } else {
                const std::ptrdiff_t i = where - order.begin();
                next = ((keyboard_.modifiers.shift ? i - 1 : i + 1) % count + count) % count;
            }
            focused_ = order[static_cast<std::size_t>(next)];
        }
    } else if (has && at->scope != 0 && !typing()) {
        // The arrows along the scope's axis, and Home and End: within it,
        // not round.
        std::vector<Id> scope;
        for (const Focusable& f : focus_order_) {
            if (f.scope == at->scope) scope.push_back(f.id);
        }
        const auto i = std::ranges::find(scope, focused_) - scope.begin();
        const bool row = at->axis == Axis::Row;
        const Key back = row ? Key::Left : Key::Up;
        const Key on = row ? Key::Right : Key::Down;
        std::ptrdiff_t next = i;
        if (usable(back)) next = i - 1;
        if (usable(on)) next = i + 1;
        if (usable(Key::Home)) next = 0;
        if (usable(Key::End)) next = static_cast<std::ptrdiff_t>(scope.size()) - 1;
        if (next >= 0 && next < static_cast<std::ptrdiff_t>(scope.size())) focused_ = scope[static_cast<std::size_t>(next)];
    }
    if (focused_ != before) {
        focused_text_ = false;
        request_frame();
    }
}

} // namespace ftk::ui::core
