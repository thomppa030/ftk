#include "imgui_harness.hpp"

#include <stdexcept>

namespace fjell::test {

ImGuiHarness::ImGuiHarness() {
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = {800.0f, 600.0f};
    io.DeltaTime = 1.0f / 60.0f;
    // Declaring texture support lets ImGui build glyphs on demand without a
    // renderer to upload them; nothing here ever draws.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // One event per frame, the way real input arrives, so a press and its
    // release land on different frames.
    io.ConfigInputTrickleEventQueue = true;
}

ImGuiHarness::~ImGuiHarness() {
    ImGui::DestroyContext(context_);
}

void ImGuiHarness::step(int count) {
    for (int i = 0; i < count; ++i) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##harness", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (ui_) ui_();
        ImGui::End();
        ImGui::Render();
    }
}

void ImGuiHarness::mark(const std::string& name) {
    marks_[name] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
}

void ImGuiHarness::click(const std::string& name) {
    auto it = marks_.find(name);
    if (it == marks_.end()) {
        throw std::runtime_error("ImGuiHarness::click: nothing marked '" + name + "'");
    }
    const ImVec2 centre{(it->second.min.x + it->second.max.x) * 0.5f,
                        (it->second.min.y + it->second.max.y) * 0.5f};
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(centre.x, centre.y);
    step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    step();
}

void ImGuiHarness::type(std::string_view text) {
    ImGui::GetIO().AddInputCharactersUTF8(std::string(text).c_str());
    step();
}

void ImGuiHarness::press(ImGuiKey key) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    step();
    io.AddKeyEvent(key, false);
    step();
}

} // namespace fjell::test
