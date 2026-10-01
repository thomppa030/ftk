#include "ftk/ui/kit/color_field.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/row.hpp"
#include "imgui_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <cstdio>
#include <string>
#include <vector>

using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

namespace {

constexpr const char* SCOPE = "1/rigidbody";

ImVec2 centre(const ImGuiHarness& h, const std::string& name) {
    const ImVec2 min = h.rect_min(name);
    const ImVec2 max = h.rect_max(name);
    return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
}

} // namespace

TEST_CASE("A finished edit is recorded under its row's label, as the field shows it", "[ui][kit]") {
    ImGuiHarness h;
    bool gravity = true;
    h.set_ui([&] {
        ui::edit_scope(SCOPE);
        if (auto t = ui::PropertyTable("##t")) {
            ui::row("Gravity", [&] { (void)ui::checkbox("##gravity", gravity); h.mark("box"); });
        }
        ui::edit_scope({});
    });
    h.step(2);
    h.click("box");
    const auto record = ui::take_edit_record(SCOPE);
    REQUIRE(record.has_value());
    CHECK(record->label == "Gravity");
    CHECK(record->before == "On");
    CHECK(record->after == "Off");
    // Taken once.
    CHECK_FALSE(ui::take_edit_record(SCOPE).has_value());
}

TEST_CASE("A drag is one record, from where it started to where it was let go", "[ui][kit]") {
    ImGuiHarness h;
    float mass = 10.0f;
    h.set_ui([&] {
        ui::edit_scope(SCOPE);
        if (auto t = ui::PropertyTable("##t")) {
            ui::row("Mass", [&] {
                (void)ui::drag("##mass", mass, {.speed = 0.1f, .unit = ui::Unit::Kilograms});
                h.mark("mass");
            });
        }
        ui::edit_scope({});
    });
    h.step(2);
    const ImVec2 from = centre(h, "mass");
    h.drag(from, {from.x + 60.0f, from.y});
    const auto record = ui::take_edit_record(SCOPE);
    REQUIRE(record.has_value());
    CHECK(record->label == "Mass");
    CHECK(record->before == "10.00 kg");
    char after[32];
    std::snprintf(after, sizeof(after), "%.2f kg", mass);
    CHECK(record->after == after);
}

TEST_CASE("A colour's typed code is recorded as the colour, not as text", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec3 tint{1.0f, 1.0f, 1.0f};
    h.set_ui([&] {
        ui::edit_scope(SCOPE);
        if (auto t = ui::PropertyTable("##t")) {
            ui::row("Tint", [&] { (void)ui::color("##tint", tint, ui::ColorSpace::Srgb); h.mark("hex"); });
        }
        ui::edit_scope({});
    });
    h.step(2);
    h.click("hex");
    for (int i = 0; i < 12; ++i) h.press(ImGuiKey_Backspace);
    h.type("#FF8000");
    h.press(ImGuiKey_Enter);
    const auto record = ui::take_edit_record(SCOPE);
    REQUIRE(record.has_value());
    CHECK(record->before == "#FFFFFF");
    CHECK(record->after == "#FF8000");
}

TEST_CASE("Two finished edits, or one outside a row or a scope, name no single field", "[ui][kit]") {
    ImGuiHarness h;
    bool a = false;
    bool b = false;
    std::vector<glm::vec3> points{{0.0f, 0.0f, 0.0f}};
    bool loose = false;
    h.set_ui([&] {
        ui::edit_scope(SCOPE);
        if (auto t = ui::PropertyTable("##t")) {
            ui::row("A", [&] { (void)ui::checkbox("##a", a); h.mark("a"); });
            ui::row("B", [&] { (void)ui::checkbox("##b", b); h.mark("b"); });
        }
        {
            ui::ListEditor list("##points", points.size());
            if (list.begin_item(0)) {
                (void)ui::checkbox("##card", loose);
                h.mark("card");
                (void)list.end_item();
            }
        }
        ui::edit_scope({});
        (void)ui::checkbox("##outside", loose);
        h.mark("outside");
    });
    h.step(2);
    h.click("a");
    h.click("b");
    CHECK_FALSE(ui::take_edit_record(SCOPE).has_value());

    h.click("card");
    CHECK_FALSE(ui::take_edit_record(SCOPE).has_value());

    h.click("outside");
    CHECK_FALSE(ui::take_edit_record(SCOPE).has_value());
    CHECK_FALSE(ui::take_edit_record("").has_value());
}
