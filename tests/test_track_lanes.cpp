#include "imgui_harness.hpp"
#include "ui/track_lanes.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::test::ImGuiHarness;

namespace {

fjell::TrackSet three_tracks() {
    fjell::TrackSet set;
    set.length = 24.0F;
    set.add("sun_intensity", fjell::TrackKind::Float, fjell::TrackMode::Multiply).curve.keyframes = {{0.5F, 1.0F, 0.0F, 0.0F}};
    set.add("sky_top", fjell::TrackKind::Color, fjell::TrackMode::Override).gradient.keys = {{0.5F, glm::vec3(1.0F)}};
    set.add("sun_color", fjell::TrackKind::Color, fjell::TrackMode::Multiply).gradient.keys = {{0.5F, glm::vec3(1.0F)}};
    return set;
}

} // namespace

TEST_CASE("Up and Down walk the lanes in their groups, not in file order", "[ui][tracks]") {
    ImGuiHarness h;
    fjell::TrackSet set = three_tracks();
    fjell::TrackLanes lanes;
    lanes.set_placement([](std::string_view name) -> fjell::TrackLanes::Placement {
        return {name.starts_with("sun") ? "Sun" : "Sky", std::string(name)};
    });
    bool focused = false;
    h.set_ui([&] {
        if (!focused) {
            ImGui::SetNextWindowFocus();
            focused = true;
        }
        (void)lanes.draw("Timeline", set, 0.0F);
    });
    h.step(2);
    CHECK(lanes.selected().empty());

    // Sun comes first, holding both sun tracks, then Sky.
    h.press(ImGuiKey_DownArrow);
    CHECK(lanes.selected() == "sun_intensity");
    h.press(ImGuiKey_DownArrow);
    CHECK(lanes.selected() == "sun_color");
    h.press(ImGuiKey_DownArrow);
    CHECK(lanes.selected() == "sky_top");
    h.press(ImGuiKey_DownArrow);
    CHECK(lanes.selected() == "sky_top");
    h.press(ImGuiKey_UpArrow);
    CHECK(lanes.selected() == "sun_color");
}
