#include "imgui_harness.hpp"
#include "ui/kit/choice.hpp"
#include "ui/kit/color_field.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <array>

using Catch::Approx;
using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

namespace {

enum class Shape { Box, Sphere, Capsule, Mesh };
constexpr ui::Choice<Shape> SHAPES[] = {
    {Shape::Box, "Box"}, {Shape::Sphere, "Sphere"}, {Shape::Capsule, "Capsule"}, {Shape::Mesh, "Mesh"},
};

enum class Body { Static, Dynamic, Kinematic };
constexpr ui::Choice<Body> BODIES[] = {
    {Body::Static, "Static"}, {Body::Dynamic, "Dynamic"}, {Body::Kinematic, "Kinematic"},
};

struct Tally {
    int changed{0};
    int committed{0};
    void add(const ui::Edit& edit) {
        if (edit.changed) ++changed;
        if (edit.committed) ++committed;
    }
};

void click_at(ImGuiHarness& h, ImVec2 at) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    h.step();
}

// The open popup's window, the one a combo or picker just opened, once it
// has had the frames it takes to size itself to its contents.
ImGuiWindow* open_popup(ImGuiHarness& h) {
    h.step(2);
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    return g.OpenPopupStack.empty() ? nullptr : g.OpenPopupStack.back().Window;
}

// The centre of the `index`th line of a popup list.
ImVec2 popup_line(const ImGuiWindow* popup, int index) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line = ImGui::GetTextLineHeight();
    return {popup->Pos.x + popup->Size.x * 0.5f,
            popup->Pos.y + style.WindowPadding.y + static_cast<float>(index) * (line + style.ItemSpacing.y)
                + line * 0.5f};
}

void erase_and_type(ImGuiHarness& h, const char* text) {
    for (int i = 0; i < 12; ++i) h.press(ImGuiKey_Backspace);
    h.type(text);
    h.press(ImGuiKey_Enter);
}

} // namespace

TEST_CASE("A drop-down sets the picked value as one commit", "[ui][kit]") {
    ImGuiHarness h;
    Shape shape = Shape::Box;
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        tally.add(ui::combo("##shape", shape, SHAPES));
        h.mark("combo");
    });
    h.step(2);
    h.click("combo");
    ImGuiWindow* popup = open_popup(h);
    REQUIRE(popup != nullptr);
    click_at(h, popup_line(popup, 2));
    CHECK(shape == Shape::Capsule);
    CHECK(tally.changed == 1);
    CHECK(tally.committed == 1);
}

TEST_CASE("Picking the value a drop-down already has is no edit", "[ui][kit]") {
    ImGuiHarness h;
    Shape shape = Shape::Sphere;
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(200.0f);
        tally.add(ui::combo("##shape", shape, SHAPES));
        h.mark("combo");
    });
    h.step(2);
    h.click("combo");
    ImGuiWindow* popup = open_popup(h);
    REQUIRE(popup != nullptr);
    click_at(h, popup_line(popup, 1));
    CHECK(shape == Shape::Sphere);
    CHECK(tally.changed == 0);
}

TEST_CASE("A segment click picks its value as one commit", "[ui][kit]") {
    ImGuiHarness h;
    Body body = Body::Static;
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(300.0f);
        tally.add(ui::segmented("##body", body, BODIES));
        h.mark("control");
    });
    h.step(2);
    const ImVec2 min = h.rect_min("control");
    const ImVec2 max = h.rect_max("control");
    const float y = (min.y + max.y) * 0.5f;
    click_at(h, {min.x + (max.x - min.x) * 5.0f / 6.0f, y});
    CHECK(body == Body::Kinematic);
    CHECK(tally.committed == 1);
    click_at(h, {min.x + (max.x - min.x) * 5.0f / 6.0f, y});
    CHECK(tally.changed == 1);
    click_at(h, {min.x + (max.x - min.x) * 0.5f, y});
    CHECK(body == Body::Dynamic);
    CHECK(tally.committed == 2);
}

TEST_CASE("A choice is segmented only for three short options that fit", "[ui][kit]") {
    ImGuiHarness h;
    const std::array<const char* const, 3> three{"Static", "Dynamic", "Kinematic"};
    const std::array<const char* const, 4> four{"Box", "Sphere", "Capsule", "Mesh"};
    bool wide = false;
    bool narrow = true;
    bool too_many = true;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(300.0f);
        wide = ui::detail::fits_segmented(three);
        ImGui::SetNextItemWidth(90.0f);
        narrow = ui::detail::fits_segmented(three);
        ImGui::SetNextItemWidth(600.0f);
        too_many = ui::detail::fits_segmented(four);
    });
    h.step(2);
    CHECK(wide);
    CHECK_FALSE(narrow);
    CHECK_FALSE(too_many);
}

TEST_CASE("A typed hex code sets a linear colour through sRGB", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec3 light{1.0f, 1.0f, 1.0f};
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(240.0f);
        tally.add(ui::color("##light", light, ui::ColorSpace::Linear));
        h.mark("hex");
    });
    h.step(2);
    h.click("hex");
    erase_and_type(h, "#FF8000");
    CHECK(light.r == Approx(1.0f));
    CHECK(light.g == Approx(0.2158f).margin(0.001f));  // sRGB 0x80 in linear light
    CHECK(light.b == Approx(0.0f));
    CHECK(tally.committed == 1);
}

TEST_CASE("A hex code that doesn't parse leaves the colour alone", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec4 tint{0.25f, 0.5f, 0.75f, 1.0f};
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(240.0f);
        tally.add(ui::color("##tint", tint, ui::ColorSpace::Srgb));
        h.mark("hex");
    });
    h.step(2);
    h.click("hex");
    erase_and_type(h, "#12G456");
    CHECK(tint == glm::vec4{0.25f, 0.5f, 0.75f, 1.0f});
    CHECK(tally.committed == 0);
}

TEST_CASE("Dragging in the colour picker commits once, on release", "[ui][kit]") {
    ImGuiHarness h;
    glm::vec3 tint{0.5f, 0.5f, 0.5f};
    Tally tally;
    h.set_ui([&] {
        ImGui::SetNextItemWidth(240.0f);
        tally.add(ui::color("##tint", tint, ui::ColorSpace::Srgb));
        h.mark("hex");
    });
    h.step(2);
    // The swatch sits left of the hex code and opens the picker.
    const ImVec2 hex = h.rect_min("hex");
    const float side = ImGui::GetFrameHeight();
    click_at(h, {hex.x - ImGui::GetStyle().ItemInnerSpacing.x - side * 0.5f, hex.y + side * 0.5f});
    ImGuiWindow* popup = open_popup(h);
    REQUIRE(popup != nullptr);
    // The saturation/value square is the picker's top-left.
    const ImVec2 corner{popup->Pos.x + ImGui::GetStyle().WindowPadding.x,
                        popup->Pos.y + ImGui::GetStyle().WindowPadding.y};
    h.drag({corner.x + 20.0f, corner.y + 20.0f}, {corner.x + 60.0f, corner.y + 50.0f});
    CHECK(tint != glm::vec3{0.5f, 0.5f, 0.5f});
    CHECK(tally.changed > 1);
    CHECK(tally.committed == 1);
}
