#include "ftk/ui/kit/asset_header.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "imgui_harness.hpp"

#include <catch2/catch_test_macros.hpp>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;

namespace {

void click_at(ImGuiHarness& h, ImVec2 p) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.y);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    h.step();
}

} // namespace

TEST_CASE("The asset header's Save, after the name, answers only while there are unsaved changes", "[ui][kit]") {
    ImGuiHarness h;
    ui::AssetHeaderSpec spec{.icon = ui::icon::material, .name = "crate_wood.fjmat", .folder = "Props / Crates",
                             .unsaved = false, .saves = "crate_wood.fjmat"};
    ui::SaveAction last = ui::SaveAction::None;
    h.set_ui([&] {
        if (auto header = ui::AssetHeaderBar(spec)) {
            // Save is the last thing the header draws before the editor's own actions.
            h.mark("save");
            header.begin_actions();
            const auto action = header.finish();
            if (action != ui::SaveAction::None) last = action;
        }
    });
    h.step(3);
    auto save = [&] {
        return ImVec2{(h.rect_min("save").x + h.rect_max("save").x) * 0.5f,
                      (h.rect_min("save").y + h.rect_max("save").y) * 0.5f};
    };
    click_at(h, save());
    CHECK(last == ui::SaveAction::None);

    spec.unsaved = true;
    h.step(2);
    click_at(h, save());
    CHECK(last == ui::SaveAction::Save);
}

TEST_CASE("A failed save shows under the header, and Try again saves", "[ui][kit]") {
    ImGuiHarness h;
    ui::AssetHeaderSpec spec{.name = "hero.fjanimset", .unsaved = true, .saves = "hero.fjanimset",
                             .error = "the animset: cannot write"};
    ui::SaveAction last = ui::SaveAction::None;
    float bottom = 0.0f;
    h.set_ui([&] {
        if (auto header = ui::AssetHeaderBar(spec)) {
            header.begin_actions();
            const auto action = header.finish();
            if (action != ui::SaveAction::None) last = action;
            bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
            h.mark("callout_end");
        }
    });
    h.step(3);
    // The strip grew to hold the callout.
    CHECK(bottom > 60.0f);
    // Try again is the callout's button, at its bottom left.
    click_at(h, {h.rect_min("callout_end").x + 40.0f, h.rect_max("callout_end").y - 12.0f});
    CHECK(last == ui::SaveAction::Save);
}
