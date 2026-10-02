#include "ftk/ui/kit/input.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <iterator>

namespace ftk::ui {

namespace {

// ImGui's key for each of ours, in Key's order.
constexpr ImGuiKey IMGUI_KEYS[] = {
    ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E, ImGuiKey_F, ImGuiKey_G, ImGuiKey_H,
    ImGuiKey_I, ImGuiKey_J, ImGuiKey_K, ImGuiKey_L, ImGuiKey_M, ImGuiKey_N, ImGuiKey_O, ImGuiKey_P,
    ImGuiKey_Q, ImGuiKey_R, ImGuiKey_S, ImGuiKey_T, ImGuiKey_U, ImGuiKey_V, ImGuiKey_W, ImGuiKey_X,
    ImGuiKey_Y, ImGuiKey_Z,
    ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7,
    ImGuiKey_8, ImGuiKey_9,
    ImGuiKey_F1, ImGuiKey_F2, ImGuiKey_F3, ImGuiKey_F4, ImGuiKey_F5, ImGuiKey_F6, ImGuiKey_F7,
    ImGuiKey_F8, ImGuiKey_F9, ImGuiKey_F10, ImGuiKey_F11, ImGuiKey_F12,
    ImGuiKey_Escape, ImGuiKey_Enter, ImGuiKey_Tab, ImGuiKey_Space, ImGuiKey_Backspace, ImGuiKey_Delete,
    ImGuiKey_Insert,
    ImGuiKey_Home, ImGuiKey_End, ImGuiKey_PageUp, ImGuiKey_PageDown, ImGuiKey_LeftArrow,
    ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
    ImGuiKey_Minus, ImGuiKey_Equal, ImGuiKey_LeftBracket, ImGuiKey_RightBracket, ImGuiKey_Backslash,
    ImGuiKey_Semicolon, ImGuiKey_Apostrophe, ImGuiKey_Comma, ImGuiKey_Period,
    ImGuiKey_Slash, ImGuiKey_GraveAccent,
};
static_assert(static_cast<std::size_t>(Key::GraveAccent) + 1 == std::size(IMGUI_KEYS));

ImGuiKey imgui(Key key) {
    return IMGUI_KEYS[static_cast<std::size_t>(key)];
}

ImGuiMouseButton imgui(MouseButton button) {
    switch (button) {
    case MouseButton::Left: return ImGuiMouseButton_Left;
    case MouseButton::Right: return ImGuiMouseButton_Right;
    case MouseButton::Middle: return ImGuiMouseButton_Middle;
    }
    return ImGuiMouseButton_Left;
}

ImGuiMouseCursor imgui(MouseCursor cursor) {
    switch (cursor) {
    case MouseCursor::Arrow: return ImGuiMouseCursor_Arrow;
    case MouseCursor::Text: return ImGuiMouseCursor_TextInput;
    case MouseCursor::Hand: return ImGuiMouseCursor_Hand;
    case MouseCursor::ResizeAll: return ImGuiMouseCursor_ResizeAll;
    case MouseCursor::ResizeEW: return ImGuiMouseCursor_ResizeEW;
    case MouseCursor::ResizeNS: return ImGuiMouseCursor_ResizeNS;
    }
    return ImGuiMouseCursor_Arrow;
}

} // namespace

bool key_pressed(Key key) {
    return ImGui::IsKeyPressed(imgui(key), false);
}

bool key_repeated(Key key) {
    return ImGui::IsKeyPressed(imgui(key), true);
}

void claim_key(Key key, const char* owner) {
    ImGui::SetKeyOwner(imgui(key), ImGui::GetID(owner));
}

Modifiers modifiers() {
    const ImGuiIO& io = ImGui::GetIO();
    return {.ctrl = io.KeyCtrl, .shift = io.KeyShift, .alt = io.KeyAlt, .super = io.KeySuper};
}

bool typing() {
    return ImGui::GetIO().WantTextInput;
}

bool any_key_pressed() {
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) return true;
    }
    return false;
}

void set_keyboard_ignored(bool ignored) {
    ImGuiIO& io = ImGui::GetIO();
    if (ignored) {
        io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoKeyboard;
    }
}

void set_clipboard(const std::string& text) {
    ImGui::SetClipboardText(text.c_str());
}

std::string clipboard() {
    const char* text = ImGui::GetClipboardText();
    return text != nullptr ? std::string(text) : std::string();
}

bool mouse_clicked(MouseButton button) {
    return ImGui::IsMouseClicked(imgui(button));
}

bool mouse_double_clicked(MouseButton button) {
    return ImGui::IsMouseDoubleClicked(imgui(button));
}

bool mouse_down(MouseButton button) {
    return ImGui::IsMouseDown(imgui(button));
}

bool mouse_released(MouseButton button) {
    return ImGui::IsMouseReleased(imgui(button));
}

bool mouse_dragging(MouseButton button) {
    return ImGui::IsMouseDragging(imgui(button), 0.0f);
}

bool any_mouse_released() {
    for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) {
        if (ImGui::IsMouseReleased(button)) return true;
    }
    return false;
}

void set_mouse_cursor(MouseCursor cursor) {
    ImGui::SetMouseCursor(imgui(cursor));
}

glm::vec2 mouse_pos() {
    return ImGui::GetMousePos();
}

bool mouse_over(glm::vec2 min, glm::vec2 max) {
    return ImGui::IsMouseHoveringRect(min, max);
}

float mouse_wheel() {
    return ImGui::GetIO().MouseWheel;
}

double seconds() {
    return ImGui::GetTime();
}

float frame_seconds() {
    return ImGui::GetIO().DeltaTime;
}

} // namespace ftk::ui
