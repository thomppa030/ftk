#include "ftk/test/imgui_harness.hpp"

#include "ftk/ui/imgui_input.hpp"
#include "ftk/ui/imgui_theme.hpp"
#include "ftk/ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <stdexcept>

namespace ftk::test {

namespace {

// Each modifier held in either.
ui::Modifiers either(ui::Modifiers a, ui::Modifiers b) {
    return {.ctrl = a.ctrl || b.ctrl, .shift = a.shift || b.shift, .alt = a.alt || b.alt, .super = a.super || b.super};
}

} // namespace

ImGuiHarness::ImGuiHarness(glm::vec2 size) : size_{size} {
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = size;
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Declaring texture support lets ImGui build glyphs on demand without a
    // renderer to upload them; nothing here ever draws.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // One event per frame, the way real input arrives, so a press and its
    // release land on different frames.
    io.ConfigInputTrickleEventQueue = true;
}

ImGuiHarness::~ImGuiHarness() {
    ftk::theme::forget_fonts(context_);
    ImGui::DestroyContext(context_);
}

void ImGuiHarness::step(int count) {
    for (int i = 0; i < count; ++i) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        // The harness is the background, as a dock host is: a window of
        // its own, such as the main menu bar, stays over it.
        ImGui::Begin("##harness", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (ui_) ui_();
        ImGui::End();
        ImGui::Render();
    }
}

void ImGuiHarness::mark(const std::string& name) {
    marks_[name] = {.min = ImGui::GetItemRectMin(), .max = ImGui::GetItemRectMax()};
}

ui::core::Rect ImGuiHarness::rect(const std::string& name) const {
    auto it = marks_.find(name);
    if (it == marks_.end()) {
        throw std::runtime_error("ImGuiHarness: nothing marked '" + name + "'");
    }
    return it->second;
}

std::optional<ui::core::Rect> ImGuiHarness::popup() const {
    const ImGuiContext& g = *context_;
    if (g.OpenPopupStack.empty() || g.OpenPopupStack.back().Window == nullptr) return std::nullopt;
    const ImGuiWindow& window = *g.OpenPopupStack.back().Window;
    return ui::core::Rect{.min = window.Pos, .max = glm::vec2(window.Pos) + glm::vec2(window.Size)};
}

std::optional<ui::core::Rect> ImGuiHarness::tooltip() const {
    // ImGui's one tooltip window, shown on the frames it is begun in.
    const ImGuiWindow* window = ImGui::FindWindowByName("##Tooltip_00");
    if (window == nullptr || !window->Active) return std::nullopt;
    return ui::core::Rect{.min = window->Pos, .max = glm::vec2(window->Pos) + glm::vec2(window->Size)};
}

void ImGuiHarness::move_to(glm::vec2 at) {
    ImGui::GetIO().AddMousePosEvent(at.x, at.y);
    step();
}

void ImGuiHarness::click(const std::string& name, ui::MouseButton button) {
    click_at(rect(name).centre(), button);
}

void ImGuiHarness::click_at(glm::vec2 at, ui::MouseButton button) {
    move_to(at);
    button_down(button);
    button_up(button);
}

void ImGuiHarness::double_click(const std::string& name) {
    click_at(rect(name).centre());
    button_down();
    button_up();
}

void ImGuiHarness::hover(const std::string& name) {
    move_to(rect(name).centre());
    step(29);
}

void ImGuiHarness::drag(glm::vec2 from, glm::vec2 to, ui::MouseButton button) {
    move_to(from);
    button_down(button);
    constexpr int STEPS = 4;
    for (int i = 1; i <= STEPS; ++i) {
        move_to(from + (to - from) * (static_cast<float>(i) / STEPS));
    }
    button_up(button);
}

void ImGuiHarness::wheel(glm::vec2 at, float notches) {
    move_to(at);
    ImGui::GetIO().AddMouseWheelEvent(0.0f, notches);
    step();
}

void ImGuiHarness::button_down(ui::MouseButton button) {
    ImGui::GetIO().AddMouseButtonEvent(ui::imgui(button), true);
    step();
}

void ImGuiHarness::button_up(ui::MouseButton button) {
    ImGui::GetIO().AddMouseButtonEvent(ui::imgui(button), false);
    step();
}

void ImGuiHarness::type(std::string_view text) {
    ImGui::GetIO().AddInputCharactersUTF8(std::string(text).c_str());
    step();
}

void ImGuiHarness::modifiers_to(ui::Modifiers modifiers) {
    ImGuiIO& io = ImGui::GetIO();
    if (modifiers.ctrl != down_.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, modifiers.ctrl);
    if (modifiers.shift != down_.shift) io.AddKeyEvent(ImGuiMod_Shift, modifiers.shift);
    if (modifiers.alt != down_.alt) io.AddKeyEvent(ImGuiMod_Alt, modifiers.alt);
    if (modifiers.super != down_.super) io.AddKeyEvent(ImGuiMod_Super, modifiers.super);
    down_ = modifiers;
}

void ImGuiHarness::press(ui::Key key, ui::Modifiers modifiers) {
    ImGuiIO& io = ImGui::GetIO();
    modifiers_to(either(modifiers, held_));
    io.AddKeyEvent(ui::imgui(key), true);
    step();
    io.AddKeyEvent(ui::imgui(key), false);
    modifiers_to(held_);
    step();
}

void ImGuiHarness::hold(ui::Modifiers modifiers) {
    held_ = modifiers;
    modifiers_to(modifiers);
    step();
}

void ImGuiHarness::key_down(ui::Key key) {
    ImGui::GetIO().AddKeyEvent(ui::imgui(key), true);
    step();
}

void ImGuiHarness::key_up(ui::Key key) {
    ImGui::GetIO().AddKeyEvent(ui::imgui(key), false);
    step();
}

ui::MouseCursor ImGuiHarness::cursor() const {
    constexpr ui::MouseCursor SHAPES[] = {ui::MouseCursor::Arrow,     ui::MouseCursor::Text,
                                          ui::MouseCursor::Hand,      ui::MouseCursor::ResizeAll,
                                          ui::MouseCursor::ResizeEW,  ui::MouseCursor::ResizeNS};
    const ImGuiMouseCursor shown = context_->MouseCursor;
    for (const ui::MouseCursor shape : SHAPES) {
        if (ui::imgui(shape) == shown) return shape;
    }
    return ui::MouseCursor::Arrow;
}

} // namespace ftk::test
