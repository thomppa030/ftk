#include "ftk/ui/kit/curve_editor.hpp"
#include "imgui_harness.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::Curve;
using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

namespace {

constexpr ui::CurveOptions OPTIONS{.size = {300.0f, 160.0f}};

Curve three_keys() {
    Curve c;
    c.keyframes = {{0.0f, 0.2f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.0f, 0.0f}, {1.0f, 0.8f, 0.0f, 0.0f}};
    return c;
}

// One editor, and where its points are on screen as last drawn.
struct Editor {
    explicit Editor(const char* name) : id{name} {}

    const char* id;
    Curve curve = three_keys();
    ui::Edit last{};
    bool committed = false;
    ImVec2 at_middle_key{};
    ImVec2 high{};   // the middle key's time at value 0.9
    ImVec2 low{};    // and at 0.1
    ImVec2 empty{};  // a quarter in, well off the curve

    void draw() {
        last = ui::curve_editor(id, curve, OPTIONS);
        committed |= last.committed;
        at_middle_key = ui::curve_editor_point(id, {curve.keyframes[1].time, curve.keyframes[1].value});
        high = ui::curve_editor_point(id, {0.5f, 0.9f});
        low = ui::curve_editor_point(id, {0.5f, 0.1f});
        empty = ui::curve_editor_point(id, {0.25f, 0.95f});
    }
};

} // namespace

TEST_CASE("The curve editor drags a key and commits once let go", "[ui][curve_editor]") {
    ImGuiHarness h;
    Editor e("##a");
    h.set_ui([&] { e.draw(); });
    h.step(2);

    h.drag(e.at_middle_key, e.high);
    CHECK(e.curve.keyframes[1].value > 0.85f);
    CHECK(e.committed);
}

TEST_CASE("Two curve editors on screen drag independently", "[ui][curve_editor]") {
    ImGuiHarness h;
    Editor first("##first");
    Editor second("##second");
    h.set_ui([&] {
        first.draw();
        second.draw();
    });
    h.step(2);

    h.drag(first.at_middle_key, first.high);
    CHECK(first.curve.keyframes[1].value > 0.85f);
    CHECK(second.curve.keyframes[1].value == 0.5f);

    h.drag(second.at_middle_key, second.low);
    CHECK(second.curve.keyframes[1].value < 0.15f);
    CHECK(first.curve.keyframes[1].value > 0.85f);
}

TEST_CASE("The curve's ends stay at 0 and 1", "[ui][curve_editor]") {
    ImGuiHarness h;
    Editor e("##ends");
    h.set_ui([&] { e.draw(); });
    h.step(2);

    ImVec2 first{};
    h.set_ui([&] {
        e.draw();
        first = ui::curve_editor_point("##ends", {0.0f, 0.2f});
    });
    h.step();
    h.drag(first, e.empty);
    CHECK(e.curve.keyframes[0].time == 0.0f);
    CHECK(e.curve.keyframes[0].value > 0.9f);
}

TEST_CASE("Double-click adds a key; right-click never removes one", "[ui][curve_editor]") {
    ImGuiHarness h;
    Editor e("##add");
    h.set_ui([&] { e.draw(); });
    h.step(2);

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(e.empty.x, e.empty.y);
    h.step();
    for (int i = 0; i < 2; ++i) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        h.step();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        h.step();
    }
    REQUIRE(e.curve.keyframes.size() == 4);
    CHECK(e.curve.keyframes[1].time > 0.2f);
    CHECK(e.curve.keyframes[1].time < 0.3f);

    io.AddMousePosEvent(e.at_middle_key.x, e.at_middle_key.y);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    h.step();
    CHECK(e.curve.keyframes.size() == 4);
}

TEST_CASE("Del removes the selected key of the editor last clicked", "[ui][curve_editor]") {
    ImGuiHarness h;
    Editor first("##del_first");
    Editor second("##del_second");
    h.set_ui([&] {
        first.draw();
        second.draw();
    });
    h.step(2);

    h.drag(first.at_middle_key, first.at_middle_key);
    h.drag(second.at_middle_key, second.at_middle_key);
    h.press(ImGuiKey_Delete);

    CHECK(second.curve.keyframes.size() == 2);
    CHECK(first.curve.keyframes.size() == 3);
}
