#include "imgui_harness.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/row.hpp"
#include "ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

namespace {

// Every frame's Edit added up over a run of frames.
struct Tally {
    int changed{0};
    int committed{0};

    void add(const ui::Edit& edit) {
        if (edit.changed) ++changed;
        if (edit.committed) ++committed;
    }
};

ImVec2 centre(const ImGuiHarness& h, const std::string& name) {
    const ImVec2 min = h.rect_min(name);
    const ImVec2 max = h.rect_max(name);
    return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
}

} // namespace

TEST_CASE("A dragged number changes live and commits once, on release", "[ui][kit]") {
    ImGuiHarness h;
    float mass = 10.0f;
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        tally.add(ui::drag("##mass", mass, {.speed = 0.1f, .unit = ui::Unit::Kilograms}));
        h.mark("mass");
    });
    h.step(2);
    const ImVec2 from = centre(h, "mass");
    h.drag(from, {from.x + 80.0f, from.y});
    CHECK(mass > 10.0f);
    CHECK(tally.changed > 1);
    CHECK(tally.committed == 1);
}

TEST_CASE("A dragged number stays inside its range", "[ui][kit]") {
    ImGuiHarness h;
    float strength = 0.9f;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        (void)ui::drag("##strength", strength, {.speed = 0.1f, .min = 0.0f, .max = 1.0f});
        h.mark("strength");
    });
    h.step(2);
    const ImVec2 from = centre(h, "strength");
    h.drag(from, {from.x + 300.0f, from.y});
    CHECK(strength == 1.0f);
}

TEST_CASE("Dragging one axis of a vector moves only that axis, as one commit", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec3 position{1.0f, 2.0f, 3.0f};
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(300.0f);
        tally.add(ui::vec3("##position", position));
        h.mark("vector");
    });
    h.step(2);
    // The middle third of the group is the Y field.
    const ImVec2 at = centre(h, "vector");
    h.drag(at, {at.x + 60.0f, at.y});
    CHECK(position.x == 1.0f);
    CHECK(position.y > 2.0f);
    CHECK(position.z == 3.0f);
    CHECK(tally.committed == 1);
}

TEST_CASE("A vector fills the width it is given", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec2 tiling{1.0f, 1.0f};
    h.set_ui([&] {
        ImGui::SetNextItemWidth(240.0f);
        (void)ui::vec2("##tiling", tiling);
        h.mark("vector");
    });
    h.step(2);
    CHECK(h.rect_max("vector").x - h.rect_min("vector").x == 240.0f);
}

TEST_CASE("A slider commits once and keeps to its ends", "[ui][kit]") {
    ImGuiHarness h;
    float slope = 45.0f;
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        tally.add(ui::slider("##slope", slope, 0.0f, 90.0f, ui::Unit::Degrees));
        h.mark("slope");
    });
    h.step(2);
    const ImVec2 from = centre(h, "slope");
    h.drag(from, {from.x + 40.0f, from.y});
    CHECK(slope > 45.0f);
    CHECK(slope < 90.0f);
    CHECK(tally.changed > 1);
    CHECK(tally.committed == 1);
    h.drag(from, {from.x + 400.0f, from.y});
    CHECK(slope == 90.0f);
    CHECK(tally.committed == 2);
}

TEST_CASE("A whole number drags in whole steps and commits once", "[ui][kit]") {
    ImGuiHarness h;
    int count = 4;
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        tally.add(ui::drag_int("##count", count, 0.5f, 0, 100));
        h.mark("count");
    });
    h.step(2);
    const ImVec2 from = centre(h, "count");
    h.drag(from, {from.x + 40.0f, from.y});
    CHECK(count > 4);
    CHECK(tally.committed == 1);
}

TEST_CASE("A checkbox commits on the click that changes it", "[ui][kit]") {
    ImGuiHarness h;
    bool shadows = false;
    Tally tally;
    h.set_ui([&] {
        tally.add(ui::checkbox("##shadows", shadows));
        h.mark("box");
    });
    h.step(2);
    h.click("box");
    CHECK(shadows);
    CHECK(tally.changed == 1);
    CHECK(tally.committed == 1);
}

TEST_CASE("Every unit but none has a symbol", "[ui][kit]") {
    CHECK(std::string(ui::unit_symbol(ui::Unit::None)).empty());
    for (auto unit : {ui::Unit::Metres, ui::Unit::SquareMetres, ui::Unit::MetresPerSecond,
                      ui::Unit::MetresPerSecondSquared,
                      ui::Unit::Degrees, ui::Unit::Radians, ui::Unit::Milliseconds, ui::Unit::Seconds, ui::Unit::Minutes,
                      ui::Unit::Kilograms, ui::Unit::Pixels, ui::Unit::Percent, ui::Unit::ExposureValue,
                      ui::Unit::Times}) {
        CHECK_FALSE(std::string(ui::unit_symbol(unit)).empty());
    }
}

TEST_CASE("A row puts its field in the value column and fills it", "[ui][kit]") {
    ImGuiHarness h;
    float mass = 1.0f;
    float window_width = 0.0f;
    h.set_ui([&] {
        window_width = ImGui::GetContentRegionAvail().x;
        if (auto t = ui::PropertyTable("##rows")) {
            ui::row("Mass", [&] {
                (void)ui::drag("##mass", mass);
                h.mark("field");
            });
            ui::hint("A hint under the row");
        }
    });
    h.step(2);
    const float label = fjell::theme::label_column(window_width);
    const float field_width = h.rect_max("field").x - h.rect_min("field").x;
    CHECK(h.rect_min("field").x >= label);
    CHECK(field_width > window_width - label - 40.0f);
}

TEST_CASE("A stepper steps one at a time and stops at its end", "[ui][kit]") {
    ImGuiHarness h;
    int size = 19;
    int commits = 0;
    h.set_ui([&] {
        if (ui::stepper("##size", size, 11, 20, ui::Unit::Pixels).committed) ++commits;
        h.mark("plus");  // the last item drawn: the plus button
    });
    h.step(2);
    h.click("plus");
    CHECK(size == 20);
    CHECK(commits == 1);
    h.click("plus");
    CHECK(size == 20);
    CHECK(commits == 1);
}
