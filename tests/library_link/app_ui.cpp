// Links ftk-app-ui alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its own
// dependencies. Running it draws a frame of the kit on ImGui with no window
// and no renderer, the way the unit tests drive editor UI.

#include "ftk/ui/kit/button.hpp"

#include <imgui.h>

int main() {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = {320.0f, 240.0f};
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    ImGui::NewFrame();
    // Sized up front: a window left to fit its contents is hidden on its
    // first frame while it measures them.
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##kit", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const bool clicked = fjell::ui::button("OK");
    ImGui::End();
    ImGui::Render();

    const bool drew = ImGui::GetDrawData()->TotalVtxCount > 0;
    ImGui::DestroyContext(context);
    return drew && !clicked ? 0 : 1;
}
