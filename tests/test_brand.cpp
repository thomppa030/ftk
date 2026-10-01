#include "ftk/test/imgui_harness.hpp"
#include "ui/brand.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

using ftk::test::ImGuiHarness;

TEST_CASE("Fjell's marks leave ImGui's stacks as they found them", "[ui][brand]") {
    ImGuiHarness h;
    int colours = -1;
    int fonts = -1;
    float drawn = 0.0f;
    h.set_ui([&] {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        const int colours_before = g.ColorStack.Size;
        const int fonts_before = g.FontStack.Size;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int vertices_before = dl->VtxBuffer.Size;
        fjell::draw_logo(dl, {110.0f, 20.0f}, 0.5f);
        fjell::wordmark(dl, {110.0f, 150.0f}, 20.0f);
        drawn = static_cast<float>(dl->VtxBuffer.Size - vertices_before);
        colours = g.ColorStack.Size - colours_before;
        fonts = g.FontStack.Size - fonts_before;
    });
    h.step(3);
    CHECK(drawn > 0.0f);
    CHECK(colours == 0);
    CHECK(fonts == 0);
    CHECK(fjell::logo_size(1.0f).x == 180.0f);
}
