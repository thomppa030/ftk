#include "imgui_harness.hpp"
#include "ui/imgui_curve_editor.hpp"

#include <catch2/catch_test_macros.hpp>

using fjell::Curve;
using fjell::CurveEditor;
using fjell::test::ImGuiHarness;

namespace {

constexpr ImVec2 SIZE{200.0f, 100.0f};

// Where a curve point sits on screen inside a marked editor, for the 0..1
// value range the editors below use.
ImVec2 point(const ImGuiHarness& h, const std::string& name, float t, float v) {
    const ImVec2 min = h.rect_min(name);
    return {min.x + t * SIZE.x, min.y + (1.0f - v) * SIZE.y};
}

Curve three_keys() {
    Curve c;
    c.keyframes = {{0.0f, 0.2f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.0f, 0.0f}, {1.0f, 0.8f, 0.0f, 0.0f}};
    return c;
}

} // namespace

TEST_CASE("CurveEditor drags a key", "[ui][curve_editor]") {
    ImGuiHarness h;
    Curve curve = three_keys();
    h.set_ui([&] { CurveEditor("##a", curve, SIZE); h.mark("a"); });
    h.step(2);

    h.drag(point(h, "a", 0.5f, 0.5f), point(h, "a", 0.5f, 0.9f));
    CHECK(curve.keyframes[1].value > 0.85f);
}

TEST_CASE("Two CurveEditors on screen drag independently", "[ui][curve_editor]") {
    ImGuiHarness h;
    Curve first = three_keys();
    Curve second = three_keys();
    h.set_ui([&] {
        CurveEditor("##first", first, SIZE);
        h.mark("first");
        CurveEditor("##second", second, SIZE);
        h.mark("second");
    });
    h.step(2);

    // The editor drawn first is the one that broke: the second reset the
    // shared drag state every frame.
    h.drag(point(h, "first", 0.5f, 0.5f), point(h, "first", 0.5f, 0.9f));
    CHECK(first.keyframes[1].value > 0.85f);
    CHECK(second.keyframes[1].value == 0.5f);

    h.drag(point(h, "second", 0.5f, 0.5f), point(h, "second", 0.5f, 0.1f));
    CHECK(second.keyframes[1].value < 0.15f);
    CHECK(first.keyframes[1].value > 0.85f);
}
