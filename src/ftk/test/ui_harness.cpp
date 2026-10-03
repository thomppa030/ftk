#include "ftk/test/ui_harness.hpp"

#include <stdexcept>
#include <vector>

namespace ftk::test {

namespace core = ui::core;

namespace {

constexpr double FRAME = 1.0 / 60.0;
constexpr core::RootId WINDOW = 1;

} // namespace

UiHarness::UiHarness(glm::vec2 size, float scale) : size_{size}, scale_{scale} {
    core::set_current(&ui_);
}

void UiHarness::step(int count) {
    for (int i = 0; i < count; ++i) {
        core::set_current(&ui_);
        time_ += FRAME;
        ui_.begin_frame(time_);
        ui_.begin_root(WINDOW, size_, scale_);
        ui_.begin_panel("##harness", {.min = {0.0f, 0.0f}, .max = size_});
        if (draw_) draw_();
        ui_.end_panel();
        ui_.end_root();
        output_ = &ui_.end_frame();
    }
}

void UiHarness::mark(const std::string& name) {
    marks_[name] = ui_.layout().last_item();
}

core::Rect UiHarness::rect(const std::string& name) const {
    if (const auto marked = marks_.find(name); marked != marks_.end()) return marked->second;
    if (const core::SemanticNode* named = ui_.find(name)) return named->rect;
    throw std::runtime_error("UiHarness: nothing marked or named '" + name + "'");
}

void UiHarness::send(core::PointerEvent event) {
    event.root = WINDOW;
    event.sample.time = time_;
    ui_.add_pointer_event(event);
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
    send({.kind = core::KeyEvent::Kind::Down, .key = key, .modifiers = modifiers});
    step();
    send({.kind = core::KeyEvent::Kind::Up, .key = key, .modifiers = modifiers});
    step();
}

void UiHarness::compose(std::string_view text) {
    ui_.set_composition(text, static_cast<int>(text.size()));
    step();
}

void UiHarness::mouse(core::PointerEvent::Kind kind, glm::vec2 at, ui::MouseButton button) {
    send({.kind = kind, .type = core::PointerType::Mouse, .button = button, .sample = {.position = at}});
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
