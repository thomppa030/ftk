#include "ftk/test/ui_harness.hpp"

#include <stdexcept>

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
    const auto found = marks_.find(name);
    if (found == marks_.end()) throw std::runtime_error("UiHarness: nothing marked '" + name + "'");
    return found->second;
}

void UiHarness::send(core::PointerEvent event) {
    event.root = WINDOW;
    event.sample.time = time_;
    ui_.add_pointer_event(event);
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

} // namespace ftk::test
