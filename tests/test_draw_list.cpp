#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

#include <cstring>
#include <functional>
#include <utility>
#include <vector>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;
namespace theme = ftk::theme;

namespace {

// What a list holds once drawn into: its vertices and indices, and each
// command's clip rectangle and length.
struct Drawn {
    std::vector<ImDrawVert> vertices;
    std::vector<ImDrawIdx> indices;
    std::vector<std::pair<ImVec4, unsigned int>> commands;
};

Drawn drawn(const ImDrawList& list) {
    Drawn d;
    d.vertices.assign(list.VtxBuffer.begin(), list.VtxBuffer.end());
    d.indices.assign(list.IdxBuffer.begin(), list.IdxBuffer.end());
    for (const ImDrawCmd& cmd : list.CmdBuffer) d.commands.emplace_back(cmd.ClipRect, cmd.ElemCount);
    return d;
}

bool same(const Drawn& a, const Drawn& b) {
    if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size()) return false;
    if (a.commands.size() != b.commands.size()) return false;
    for (size_t i = 0; i < a.commands.size(); ++i) {
        const auto& [clip_a, count_a] = a.commands[i];
        const auto& [clip_b, count_b] = b.commands[i];
        if (count_a != count_b || clip_a.x != clip_b.x || clip_a.y != clip_b.y || clip_a.z != clip_b.z ||
            clip_a.w != clip_b.w) {
            return false;
        }
    }
    return std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(ImDrawVert)) == 0 &&
           std::memcmp(a.indices.data(), b.indices.data(), a.indices.size() * sizeof(ImDrawIdx)) == 0;
}

// Draws into a window of its own at the same place every time, with nothing
// of the window's own in its list, and returns what the list holds.
Drawn draw_in_window(const char* name, const std::function<void()>& draw) {
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({400.0f, 300.0f});
    ImGui::Begin(name, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings);
    draw();
    Drawn d = drawn(*ImGui::GetWindowDrawList());
    ImGui::End();
    return d;
}

ImU32 packed(ui::Color colour) {
    return ImGui::GetColorU32(colour);
}

} // namespace

TEST_CASE("A draw list draws what ImGui's would for the same shapes", "[ui][draw_list]") {
    const glm::vec2 points[] = {{20.0f, 200.0f}, {60.0f, 240.0f}, {100.0f, 210.0f}};
    Drawn facade;
    Drawn imgui;
    ImGuiHarness h;
    h.set_ui([&] {
        facade = draw_in_window("##facade", [&] {
            ui::DrawList dl = ui::DrawList::window();
            dl.rect_filled({10.0f, 10.0f}, {90.0f, 40.0f}, theme::surface_sunken(), 4.0f, ui::RoundCorners::Top);
            dl.rect({10.0f, 50.0f}, {90.0f, 80.0f}, theme::border(), 3.0f, ui::RoundCorners::None, 1.5f);
            dl.line({100.0f, 10.0f}, {180.0f, 60.0f}, theme::accent(), 2.0f);
            dl.polyline(points, theme::text_secondary(), 2.0f, true);
            dl.circle_filled({220.0f, 40.0f}, 12.0f, theme::success());
            dl.circle({260.0f, 40.0f}, 12.0f, theme::warning(), 2.0f);
            dl.arc({300.0f, 40.0f}, 12.0f, 0.0f, 3.0f, theme::accent(), 2.5f);
            dl.triangle_filled({10.0f, 100.0f}, {40.0f, 100.0f}, {25.0f, 120.0f}, theme::error());
            dl.triangle({50.0f, 100.0f}, {80.0f, 100.0f}, {65.0f, 120.0f}, theme::text(), 1.5f);
            dl.quad_filled({90.0f, 100.0f}, {110.0f, 90.0f}, {130.0f, 100.0f}, {110.0f, 110.0f}, theme::accent());
            dl.quad({140.0f, 100.0f}, {160.0f, 90.0f}, {180.0f, 100.0f}, {160.0f, 110.0f}, theme::text(), 1.0f);
            dl.push_clip({0.0f, 130.0f}, {150.0f, 160.0f});
            dl.text({10.0f, 135.0f}, theme::text(), "Opening harbour");
            dl.pop_clip();
        });
        imgui = draw_in_window("##imgui", [&] {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled({10.0f, 10.0f}, {90.0f, 40.0f}, packed(theme::surface_sunken()), 4.0f,
                              ImDrawFlags_RoundCornersTop);
            dl->AddRect({10.0f, 50.0f}, {90.0f, 80.0f}, packed(theme::border()), 3.0f, ImDrawFlags_RoundCornersNone,
                        1.5f);
            dl->AddLine({100.0f, 10.0f}, {180.0f, 60.0f}, packed(theme::accent()), 2.0f);
            const ImVec2 imgui_points[] = {points[0], points[1], points[2]};
            dl->AddPolyline(imgui_points, 3, packed(theme::text_secondary()), ImDrawFlags_Closed, 2.0f);
            dl->AddCircleFilled({220.0f, 40.0f}, 12.0f, packed(theme::success()));
            dl->AddCircle({260.0f, 40.0f}, 12.0f, packed(theme::warning()), 0, 2.0f);
            dl->PathArcTo({300.0f, 40.0f}, 12.0f, 0.0f, 3.0f);
            dl->PathStroke(packed(theme::accent()), ImDrawFlags_None, 2.5f);
            dl->AddTriangleFilled({10.0f, 100.0f}, {40.0f, 100.0f}, {25.0f, 120.0f}, packed(theme::error()));
            dl->AddTriangle({50.0f, 100.0f}, {80.0f, 100.0f}, {65.0f, 120.0f}, packed(theme::text()), 1.5f);
            dl->AddQuadFilled({90.0f, 100.0f}, {110.0f, 90.0f}, {130.0f, 100.0f}, {110.0f, 110.0f},
                              packed(theme::accent()));
            dl->AddQuad({140.0f, 100.0f}, {160.0f, 90.0f}, {180.0f, 100.0f}, {160.0f, 110.0f}, packed(theme::text()),
                        1.0f);
            dl->PushClipRect({0.0f, 130.0f}, {150.0f, 160.0f}, true);
            dl->AddText({10.0f, 135.0f}, packed(theme::text()), "Opening harbour");
            dl->PopClipRect();
        });
    });
    h.step(2);

    REQUIRE_FALSE(facade.vertices.empty());
    CHECK(same(facade, imgui));
}

TEST_CASE("A draw list rounds the corners it is asked to", "[ui][draw_list]") {
    const std::pair<ui::RoundCorners, ImDrawFlags> corners[] = {
        {ui::RoundCorners::All, ImDrawFlags_RoundCornersAll},
        {ui::RoundCorners::Top, ImDrawFlags_RoundCornersTop},
        {ui::RoundCorners::Bottom, ImDrawFlags_RoundCornersBottom},
        {ui::RoundCorners::Left, ImDrawFlags_RoundCornersLeft},
        {ui::RoundCorners::Right, ImDrawFlags_RoundCornersRight},
        {ui::RoundCorners::TopLeft, ImDrawFlags_RoundCornersTopLeft},
        {ui::RoundCorners::TopRight, ImDrawFlags_RoundCornersTopRight},
        {ui::RoundCorners::BottomLeft, ImDrawFlags_RoundCornersBottomLeft},
        {ui::RoundCorners::BottomRight, ImDrawFlags_RoundCornersBottomRight},
        {ui::RoundCorners::None, ImDrawFlags_RoundCornersNone},
    };
    std::vector<std::pair<Drawn, Drawn>> rects;
    ImGuiHarness h;
    h.set_ui([&] {
        rects.clear();
        for (const auto& [round, flags] : corners) {
            Drawn facade = draw_in_window("##facade", [&] {
                ui::DrawList::window().rect_filled({10.0f, 10.0f}, {90.0f, 40.0f}, theme::accent(), 6.0f, round);
            });
            Drawn imgui = draw_in_window("##imgui", [&] {
                ImGui::GetWindowDrawList()->AddRectFilled({10.0f, 10.0f}, {90.0f, 40.0f}, packed(theme::accent()),
                                                          6.0f, flags);
            });
            rects.emplace_back(std::move(facade), std::move(imgui));
        }
    });
    h.step(2);

    REQUIRE(rects.size() == std::size(corners));
    for (const auto& [facade, imgui] : rects) CHECK(same(facade, imgui));
}

TEST_CASE("A draw list's clip falls within the one already there", "[ui][draw_list]") {
    glm::vec2 inner_min{};
    glm::vec2 inner_max{};
    glm::vec2 after_min{};
    glm::vec2 outer_min{};
    ImGuiHarness h;
    h.set_ui([&] {
        draw_in_window("##clip", [&] {
            ui::DrawList dl = ui::DrawList::window();
            dl.push_clip({20.0f, 20.0f}, {200.0f, 200.0f});
            outer_min = dl.clip_min();
            dl.push_clip({0.0f, 50.0f}, {100.0f, 300.0f});
            inner_min = dl.clip_min();
            inner_max = dl.clip_max();
            dl.pop_clip();
            after_min = dl.clip_min();
            dl.pop_clip();
        });
    });
    h.step(2);

    CHECK(outer_min == glm::vec2{20.0f, 20.0f});
    CHECK(inner_min == glm::vec2{20.0f, 50.0f});
    CHECK(inner_max == glm::vec2{100.0f, 200.0f});
    CHECK(after_min == outer_min);
}
