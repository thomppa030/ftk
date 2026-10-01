#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/search.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <string>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;

TEST_CASE("A search matches anywhere in the text, ignoring case", "[ui][kit]") {
    CHECK(ui::matches("Crate_01", "cra"));
    CHECK(ui::matches("Crate_01", "TE_0"));
    CHECK(ui::matches("Crate_01", ""));
    CHECK_FALSE(ui::matches("Crate_01", "box"));
    CHECK(ui::detail::find_match("Old crate", "CRATE") == 4);
}

TEST_CASE("The search field filters as you type, and its clear button empties it", "[ui][kit]") {
    ImGuiHarness h;
    std::string query;
    int changes = 0;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        if (ui::search_field("##search", query)) ++changes;
        h.mark("field");
    });
    h.step(2);
    h.click("field");
    h.type("cra");
    // While typing, with no Enter.
    CHECK(query == "cra");
    CHECK(changes > 0);

    // The clear button sits over the right end of the field.
    const ImVec2 min = h.rect_min("field");
    const ImVec2 max = h.rect_max("field");
    const float side = max.y - min.y;
    const ImVec2 clear{max.x - side * 0.5f, (min.y + max.y) * 0.5f};
    h.drag(clear, clear);
    CHECK(query.empty());
}

TEST_CASE("Esc empties the search field, not only the typing since it was entered", "[ui][kit]") {
    ImGuiHarness h;
    std::string query = "lamp";
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        (void)ui::search_field("##search", query);
        h.mark("field");
    });
    h.step(2);
    h.click("field");
    h.type("s");
    REQUIRE(query == "lamps");
    h.press(ImGuiKey_Escape);
    CHECK(query.empty());
}

TEST_CASE("Ctrl F puts the cursor in the focused window's search field", "[ui][kit]") {
    ImGuiHarness h;
    std::string query;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        (void)ui::search_field("##search", query);
    });
    h.step(2);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    h.press(ImGuiKey_F);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    h.step(1);
    h.type("x");
    CHECK(query == "x");
}

TEST_CASE("Ctrl F reaches the search field without a navigation ring around it", "[ui][kit]") {
    ImGuiHarness h;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    std::string query;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        (void)ui::search_field("##search", query);
    });
    h.step(2);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    h.press(ImGuiKey_F);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    h.step(2);
    REQUIRE(ImGui::GetCurrentContext()->ActiveId != 0);
    CHECK_FALSE(ImGui::GetCurrentContext()->NavCursorVisible);
}
