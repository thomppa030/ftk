#include "imgui_harness.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/list_editor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using fjell::test::ImGuiHarness;
namespace ui = fjell::ui;

namespace {

// A list_editor of numbers in the harness. Each card marks its content; the
// list's right edge is kept to aim at the trash icons.
struct Fixture {
    ImGuiHarness h;
    std::vector<int> items{10, 20, 30};
    int commits{0};
    float right{0.0f};

    Fixture() {
        h.set_ui([this] {
            right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            const ui::Edit edit = ui::list_editor(
                "##numbers", items, "Add", "Nothing",
                [this](int&, std::size_t i) {
                    ImGui::TextUnformatted("item");
                    h.mark("content" + std::to_string(i));
                    return ui::Edit{};
                });
            h.mark("add");
            if (edit.committed) ++commits;
        });
        h.step(3);
    }

    // The centre of card `i`'s trash icon: the frame-height square at the
    // card's right edge, inset by the card's padding.
    [[nodiscard]] ImVec2 trash(int i) const {
        const float side = ImGui::GetFrameHeight();
        const float top = h.rect_min("content" + std::to_string(i)).y;
        return {right - 4.0f - side * 0.5f, top + side * 0.5f};
    }
    // A point on card `i`'s grip, left of its number and content.
    [[nodiscard]] ImVec2 grip(int i) const {
        const ImVec2 content = h.rect_min("content" + std::to_string(i));
        return {content.x - 36.0f, content.y + 8.0f};
    }
    // Card `i`'s top and bottom.
    [[nodiscard]] float top(int i) const { return h.rect_min("content" + std::to_string(i)).y - 4.0f; }
    [[nodiscard]] float bottom(int i) const { return top(i) + 4.0f + ImGui::GetFrameHeight() + 4.0f; }
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

} // namespace

TEST_CASE("A list's trash icon removes that item, as a finished edit", "[ui][kit]") {
    Fixture f;
    click_at(f.h, f.trash(1));
    CHECK(f.items == std::vector{10, 30});
    CHECK(f.commits == 1);
}

TEST_CASE("A list's Add button appends an item, as a finished edit", "[ui][kit]") {
    Fixture f;
    f.h.click("add");
    CHECK(f.items == std::vector{10, 20, 30, 0});
    CHECK(f.commits == 1);
}

TEST_CASE("Dragging a card onto the lower half of a later one moves it after that one", "[ui][kit]") {
    Fixture f;
    const ImVec2 grip = f.grip(0);
    f.h.drag(grip, {grip.x + 20.0f, f.bottom(2) - 3.0f});
    CHECK(f.items == std::vector{20, 30, 10});
    CHECK(f.commits == 1);
}

TEST_CASE("Dragging a card onto the upper half of an earlier one moves it before that one",
          "[ui][kit]") {
    Fixture f;
    const ImVec2 grip = f.grip(2);
    f.h.drag(grip, {grip.x + 20.0f, f.top(1) + 3.0f});
    CHECK(f.items == std::vector{10, 30, 20});
    CHECK(f.commits == 1);
}

TEST_CASE("Dropping a card where it already is changes nothing", "[ui][kit]") {
    Fixture f;
    const ImVec2 grip = f.grip(1);
    f.h.drag(grip, {grip.x + 20.0f, f.bottom(1) - 3.0f});
    CHECK(f.items == std::vector{10, 20, 30});
    CHECK(f.commits == 0);
}
