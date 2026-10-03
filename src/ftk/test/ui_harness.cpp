#include "ftk/test/ui_harness.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ftk::test {

namespace core = ui::core;

namespace {

constexpr double FRAME = 1.0 / 60.0;
// A held key repeats after this long, and then this often, as a keyboard
// does by default.
constexpr double REPEAT_DELAY = 0.275;
constexpr double REPEAT_RATE = 0.05;

// Each modifier held in either.
ui::Modifiers either(ui::Modifiers a, ui::Modifiers b) {
    return {.ctrl = a.ctrl || b.ctrl, .shift = a.shift || b.shift, .alt = a.alt || b.alt, .super = a.super || b.super};
}

} // namespace

UiHarness::UiHarness(glm::vec2 size, float scale, bool panel) : size_{size}, scale_{scale}, panel_{panel} {
    core::set_current(&ui_);
}

void UiHarness::step(int count) {
    for (int i = 0; i < count; ++i) {
        core::set_current(&ui_);
        time_ += FRAME;
        // The keys held down repeat, as the system sends them.
        for (auto& [key, next] : keys_held_) {
            if (time_ < next) continue;
            send({.kind = core::KeyEvent::Kind::Down, .key = key, .repeat = true, .modifiers = held_});
            next += REPEAT_RATE;
        }
        ui_.begin_frame(time_);
        ui_.begin_root(WINDOW, size_, scale_);
        if (panel_) ui_.begin_panel("##harness", {.min = {0.0f, 0.0f}, .max = size_});
        if (draw_) draw_();
        if (panel_) ui_.end_panel();
        ui_.end_root();
        // The windows opened for the UI, drawn after, as a program draws
        // each of its windows' roots.
        for (const core::RootRequest& window : windows_) {
            ui_.begin_root(window.root, window.size, scale_);
            ui_.end_root();
        }
        output_ = &ui_.end_frame();
        // Asked for again, a window is moved and sized as asked.
        for (const core::RootRequest& opened : output_->open) {
            const auto known = std::ranges::find(windows_, opened.root, &core::RootRequest::root);
            if (known != windows_.end()) {
                *known = opened;
            } else {
                windows_.push_back(opened);
            }
        }
        for (const core::RootId closed : output_->close) {
            std::erase_if(windows_, [&](const core::RootRequest& window) { return window.root == closed; });
            ui_.forget_root(closed);
        }
    }
}

void UiHarness::mark(const std::string& name) {
    marks_[name] = ui_.layout().last_item();
}

void UiHarness::mark(const std::string& name, core::Rect rect) {
    marks_[name] = rect;
}

core::Rect UiHarness::rect(const std::string& name) const {
    if (const auto marked = marks_.find(name); marked != marks_.end()) return marked->second;
    if (const core::SemanticNode* named = ui_.find(name)) return named->rect;
    throw std::runtime_error("UiHarness: nothing marked or named '" + name + "'");
}

std::optional<core::Rect> UiHarness::popup() const {
    // Popups are recorded as they are drawn, one opened from another after it.
    std::optional<core::Rect> top;
    for (const core::SemanticNode& node : ui_.semantics()) {
        if (node.role == core::Role::Popup) top = node.rect;
    }
    return top;
}

std::optional<core::Rect> UiHarness::tooltip() const {
    for (const core::SemanticNode& node : ui_.semantics()) {
        if (node.role == core::Role::Tooltip) return node.rect;
    }
    return std::nullopt;
}

void UiHarness::send(core::PointerEvent event) {
    // In the harness's own window unless it says another.
    if (event.root == 0) event.root = WINDOW;
    event.sample.time = time_;
    ui_.add_pointer_event(event);
}

void UiHarness::close_window(core::RootId root) {
    std::erase_if(windows_, [&](const core::RootRequest& window) { return window.root == root; });
    ui_.closed_root(root);
}

void UiHarness::click_in(core::RootId root, glm::vec2 at) {
    send({.kind = core::PointerEvent::Kind::Move, .root = root, .sample = {.position = at}});
    step();
    send({.kind = core::PointerEvent::Kind::Down, .root = root, .button = ui::MouseButton::Left, .sample = {.position = at}});
    step();
    send({.kind = core::PointerEvent::Kind::Up, .root = root, .button = ui::MouseButton::Left, .sample = {.position = at}});
    step();
}

void UiHarness::send(core::KeyEvent event) {
    event.root = WINDOW;
    ui_.add_key_event(event);
}

void UiHarness::type(std::string_view text) {
    ui_.add_text(text);
    step();
}

void UiHarness::press(ui::Key key, ui::Modifiers modifiers) {
    const ui::Modifiers held = either(modifiers, held_);
    send({.kind = core::KeyEvent::Kind::Down, .key = key, .modifiers = held});
    step();
    send({.kind = core::KeyEvent::Kind::Up, .key = key, .modifiers = held});
    step();
}

void UiHarness::hold(ui::Modifiers modifiers) {
    held_ = modifiers;
    send({.kind = core::KeyEvent::Kind::Modifiers, .modifiers = modifiers});
    step();
}

void UiHarness::key_down(ui::Key key) {
    send({.kind = core::KeyEvent::Kind::Down, .key = key, .modifiers = held_});
    keys_held_[key] = time_ + REPEAT_DELAY;
    step();
}

void UiHarness::key_up(ui::Key key) {
    keys_held_.erase(key);
    send({.kind = core::KeyEvent::Kind::Up, .key = key, .modifiers = held_});
    step();
}

ui::MouseCursor UiHarness::cursor() const {
    if (output_ == nullptr || output_->roots.empty()) return ui::MouseCursor::Arrow;
    return output_->roots.front().cursor;
}

void UiHarness::compose(std::string_view text) {
    ui_.set_composition(text, static_cast<int>(text.size()));
    step();
}

void UiHarness::mouse(core::PointerEvent::Kind kind, glm::vec2 at, ui::MouseButton button) {
    mouse_at_ = at;
    send({.kind = kind, .type = core::PointerType::Mouse, .button = button, .sample = {.position = at}});
}

void UiHarness::button_down(ui::MouseButton button) {
    mouse(core::PointerEvent::Kind::Down, mouse_at_, button);
    step();
}

void UiHarness::button_up(ui::MouseButton button) {
    mouse(core::PointerEvent::Kind::Up, mouse_at_, button);
    step();
}

void UiHarness::move_to(glm::vec2 at) {
    mouse(core::PointerEvent::Kind::Move, at);
    step();
}

void UiHarness::click(const std::string& name, ui::MouseButton button) {
    click_at(rect(name).centre(), button);
}

void UiHarness::click_at(glm::vec2 at, ui::MouseButton button) {
    move_to(at);
    mouse(core::PointerEvent::Kind::Down, at, button);
    step();
    mouse(core::PointerEvent::Kind::Up, at, button);
    step();
}

void UiHarness::double_click(const std::string& name) {
    const glm::vec2 at = rect(name).centre();
    click_at(at);
    mouse(core::PointerEvent::Kind::Down, at);
    step();
    mouse(core::PointerEvent::Kind::Up, at);
    step();
}

void UiHarness::hover(const std::string& name) {
    move_to(rect(name).centre());
    step(30);
}

void UiHarness::drag(glm::vec2 from, glm::vec2 to, ui::MouseButton button) {
    move_to(from);
    mouse(core::PointerEvent::Kind::Down, from, button);
    step();
    constexpr int STEPS = 4;
    for (int i = 1; i <= STEPS; ++i) {
        const float t = static_cast<float>(i) / STEPS;
        mouse(core::PointerEvent::Kind::Move, from + (to - from) * t);
        step();
    }
    mouse(core::PointerEvent::Kind::Up, to, button);
    step();
}

void UiHarness::wheel(glm::vec2 at, float notches) {
    move_to(at);
    send({.kind = core::PointerEvent::Kind::Wheel,
          .type = core::PointerType::Mouse,
          .sample = {.position = at},
          .wheel = {0.0f, notches}});
    step();
}

void UiHarness::finger(core::PointerEvent::Kind kind, core::PointerId finger, glm::vec2 at) {
    send({.kind = kind, .type = core::PointerType::Touch, .id = finger, .sample = {.position = at}});
}

void UiHarness::tap(const std::string& name) {
    tap_at(rect(name).centre());
}

void UiHarness::tap_at(glm::vec2 at, int fingers) {
    std::vector<core::PointerId> down;
    std::vector<glm::vec2> where;
    for (int i = 0; i < fingers; ++i) {
        down.push_back(next_finger());
        where.push_back(at + glm::vec2{20.0f * (static_cast<float>(i) - 0.5f * static_cast<float>(fingers - 1)), 0.0f});
        finger(core::PointerEvent::Kind::Down, down.back(), where.back());
    }
    step();
    for (std::size_t i = 0; i < down.size(); ++i) finger(core::PointerEvent::Kind::Up, down[i], where[i]);
    step();
}

void UiHarness::long_press(const std::string& name, double seconds) {
    const glm::vec2 at = rect(name).centre();
    const core::PointerId held = next_finger();
    finger(core::PointerEvent::Kind::Down, held, at);
    const double until = time_ + seconds;
    do {
        step();
    } while (time_ < until);
    finger(core::PointerEvent::Kind::Up, held, at);
    step();
}

void UiHarness::pen_stroke(std::span<const glm::vec2> points, float pressure) {
    if (points.empty()) return;
    constexpr core::PointerId PEN = 1;
    const auto pen = [&](core::PointerEvent::Kind kind, glm::vec2 at, float pressed) {
        send({.kind = kind, .type = core::PointerType::Pen, .id = PEN, .sample = {.position = at, .pressure = pressed}});
    };
    pen(core::PointerEvent::Kind::Move, points.front(), 0.0f);
    step();
    pen(core::PointerEvent::Kind::Down, points.front(), pressure);
    step();
    for (const glm::vec2 point : points.subspan(1)) {
        pen(core::PointerEvent::Kind::Move, point, pressure);
        step();
    }
    pen(core::PointerEvent::Kind::Up, points.back(), 0.0f);
    step();
    pen(core::PointerEvent::Kind::Leave, points.back(), 0.0f);
    step();
}

void UiHarness::pinch(glm::vec2 centre, float from, float to) {
    const core::PointerId a = next_finger();
    const core::PointerId b = next_finger();
    const auto apart = [&](float distance) { return glm::vec2{0.5f * distance, 0.0f}; };
    finger(core::PointerEvent::Kind::Down, a, centre - apart(from));
    finger(core::PointerEvent::Kind::Down, b, centre + apart(from));
    step();
    constexpr int STEPS = 4;
    for (int i = 1; i <= STEPS; ++i) {
        const float distance = from + (to - from) * static_cast<float>(i) / STEPS;
        finger(core::PointerEvent::Kind::Move, a, centre - apart(distance));
        finger(core::PointerEvent::Kind::Move, b, centre + apart(distance));
        step();
    }
    finger(core::PointerEvent::Kind::Up, a, centre - apart(to));
    finger(core::PointerEvent::Kind::Up, b, centre + apart(to));
    step();
}

} // namespace ftk::test
