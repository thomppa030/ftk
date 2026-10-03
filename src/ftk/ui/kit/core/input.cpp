#include "ftk/ui/kit/input.hpp"

#include "ftk/ui/kit/core/kit.hpp"

#include <glm/geometric.hpp>

namespace ftk::ui {

using namespace corekit;

namespace {

// The mouse, as the frame sees it; none until it has moved.
const core::Pointer* mouse() {
    return ui().find_pointer(core::PointerType::Mouse, 0);
}

bool has(uint8_t bits, MouseButton button) {
    return (bits & core::button_bit(button)) != 0;
}

} // namespace

bool key_pressed(Key key) {
    return ui().key_pressed(key);
}

bool key_repeated(Key key) {
    return ui().key_repeated(key);
}

void claim_key(Key key, const char* owner) {
    ui().claim_key(key, ui().id(owner));
}

Modifiers modifiers() {
    return ui().modifiers();
}

bool typing() {
    // As ImGui's WantTextInput: a field that had the keyboard as the frame
    // began still holds this frame's keys, though Enter let it go.
    return ui().typing() || ui().typing_as_begun();
}

bool any_key_pressed() {
    return ui().any_key_pressed();
}

void set_keyboard_ignored(bool ignored) {
    ui().set_keyboard_ignored(ignored);
}

void set_clipboard(const std::string& text) {
    ui().set_clipboard(text);
}

std::string clipboard() {
    return ui().clipboard();
}

bool mouse_clicked(MouseButton button) {
    const core::Pointer* m = mouse();
    return m != nullptr && has(m->pressed, button);
}

bool mouse_double_clicked(MouseButton button) {
    const core::Pointer* m = mouse();
    return m != nullptr && has(m->pressed, button) && m->click_count[static_cast<std::size_t>(button)] == 2;
}

bool mouse_down(MouseButton button) {
    const core::Pointer* m = mouse();
    return m != nullptr && has(m->buttons, button);
}

bool mouse_released(MouseButton button) {
    const core::Pointer* m = mouse();
    return m != nullptr && has(m->released, button);
}

bool mouse_dragging(MouseButton button) {
    const core::Pointer* m = mouse();
    if (m == nullptr || !has(m->buttons, button)) return false;
    return glm::distance(m->now.position, m->press_position[static_cast<std::size_t>(button)]) > 0.0f;
}

bool any_mouse_released() {
    const core::Pointer* m = mouse();
    return m != nullptr && m->released != 0;
}

void set_mouse_cursor(MouseCursor cursor) {
    ui().set_cursor_shape(cursor);
}

glm::vec2 mouse_pos() {
    const core::Pointer* m = mouse();
    return m != nullptr ? m->now.position : glm::vec2{-1.0f};
}

bool mouse_over(glm::vec2 min, glm::vec2 max) {
    const core::Pointer* m = mouse();
    if (m == nullptr || !m->present) return false;
    const core::DrawList& list = ui().draw_list();
    const core::Rect within = core::Rect{.min = min, .max = max}.intersect({.min = list.clip_min(), .max = list.clip_max()});
    return within.contains(m->now.position);
}

float mouse_wheel() {
    const core::Pointer* m = mouse();
    return m != nullptr ? m->wheel.y : 0.0f;
}

glm::vec2 mouse_delta() {
    const core::Pointer* m = mouse();
    return m != nullptr ? m->now.position - m->previous : glm::vec2{0.0f};
}

double seconds() {
    return ui().time();
}

float frame_seconds() {
    return ui().delta();
}

uint64_t frame_count() {
    return ui().frame();
}

} // namespace ftk::ui
