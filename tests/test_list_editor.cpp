#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/theme.hpp"
#include "imgui_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
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
        const float top = line_top(i);
        return {right - 4.0f - side * 0.5f, top + side * 0.5f};
    }
    // A point on card `i`'s grip, left of its number and content.
    [[nodiscard]] ImVec2 grip(int i) const {
        const ImVec2 content = h.rect_min("content" + std::to_string(i));
        return {content.x - 36.0f, content.y + 8.0f};
    }
    // The top of card `i`'s first line: its text sits a frame's padding
    // below it.
    [[nodiscard]] float line_top(int i) const {
        return h.rect_min("content" + std::to_string(i)).y - ImGui::GetStyle().FramePadding.y;
    }
    // Card `i`'s top and bottom.
    [[nodiscard]] float top(int i) const { return line_top(i) - 4.0f; }
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

namespace {

// A selectable list of numbers: each card counts how often its summary and
// its fields are drawn, and marks its summary line.
struct SelectableFixture {
    ImGuiHarness h;
    std::vector<int> items{10, 20, 30};
    int selected{-1};
    int fields_drawn{0};
    int summaries_drawn{0};

    SelectableFixture() {
        h.set_ui([this] {
            (void)ui::selectable_list_editor(
                "##select", items, selected, "Add", "Nothing", {},
                [this](const int&, std::size_t i) {
                    ++summaries_drawn;
                    ImGui::TextUnformatted("summary");
                    h.mark("summary" + std::to_string(i));
                },
                [this](int&, std::size_t i) {
                    ++fields_drawn;
                    ImGui::TextUnformatted("fields");
                    h.mark("summary" + std::to_string(i));
                    return ui::Edit{};
                },
                [] { return 0; });
        });
        h.step(3);
    }
};

} // namespace

TEST_CASE("Clicking a card selects it, and clicking it again lets it go", "[ui][kit]") {
    SelectableFixture f;
    CHECK(f.selected == -1);
    f.h.click("summary1");
    CHECK(f.selected == 1);
    f.h.click("summary1");
    CHECK(f.selected == -1);
}

TEST_CASE("Only the selected card draws its fields", "[ui][kit]") {
    SelectableFixture f;
    f.h.click("summary2");
    f.fields_drawn = 0;
    f.summaries_drawn = 0;
    f.h.step();
    CHECK(f.fields_drawn == 1);
    CHECK(f.summaries_drawn == 2);
}

TEST_CASE("Removing a card before the selected one keeps the selection on its item", "[ui][kit]") {
    SelectableFixture f;
    f.h.click("summary2");
    // The trash icon of card 0: the frame-height square at the card's right.
    const ImVec2 min = f.h.rect_min("summary0");
    const float side = ImGui::GetFrameHeight();
    const float right = ImGui::GetMainViewport()->Size.x - ImGui::GetStyle().WindowPadding.x;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(right - 4.0f - side * 0.5f, min.y + side * 0.5f);
    f.h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    f.h.step();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    f.h.step();
    CHECK(f.items == std::vector{20, 30});
    CHECK(f.selected == 1);
}

TEST_CASE("Cards out of sight are not drawn", "[ui][kit]") {
    ImGuiHarness h;
    std::vector<int> items(500, 0);
    int selected = -1;
    int drawn = 0;
    h.set_ui([&] {
        (void)ui::selectable_list_editor(
            "##long", items, selected, "Add", "Nothing", {.max_cards = 8},
            [&](const int&, std::size_t) { ++drawn; },
            [&](int&, std::size_t) { ++drawn; return ui::Edit{}; }, [] { return 0; });
    });
    h.step(3);
    drawn = 0;
    h.step();
    CHECK(drawn > 0);
    CHECK(drawn <= 10);
}

TEST_CASE("An index follows its item when another moves past it", "[ui][kit]") {
    // Item 0 moved in front of 3: items 1 and 2 shift down.
    CHECK(ui::index_after_move(0, 0, 3) == 2);
    CHECK(ui::index_after_move(1, 0, 3) == 0);
    CHECK(ui::index_after_move(3, 0, 3) == 3);
    // Item 3 moved in front of 1: items 1 and 2 shift up.
    CHECK(ui::index_after_move(3, 3, 1) == 1);
    CHECK(ui::index_after_move(1, 3, 1) == 2);
    CHECK(ui::index_after_move(0, 3, 1) == 0);
}

TEST_CASE("A card whose first line is text centres it on the number's line", "[ui][kit]") {
    ImGuiHarness h;
    std::vector<int> items{10};
    float list_top = 0.0f;
    h.set_ui([&] {
        list_top = ImGui::GetCursorScreenPos().y;
        (void)ui::list_editor("##numbers", items, "Add", "Nothing", [&](int&, std::size_t) {
            ImGui::TextUnformatted("Sphere on Head");
            h.mark("text");
            return ui::Edit{};
        });
    });
    h.step(3);
    // The number, grip and trash icon sit on a field line under the card's
    // 4 px padding; the text's middle is that line's middle.
    const float text_middle = (h.rect_min("text").y + h.rect_max("text").y) * 0.5f;
    const float line_middle = list_top + 4.0f + ImGui::GetFrameHeight() * 0.5f;
    CHECK(std::abs(text_middle - line_middle) <= 1.0f);
}

TEST_CASE("A closed card that opens reads dim, and in full text under the mouse", "[ui][kit]") {
    ImGuiHarness h;
    std::vector<int> items{10, 20};
    int selected = 0;
    ImVec4 summary_colour{};
    h.set_ui([&] {
        (void)ui::selectable_list_editor(
            "##points", items, selected, "Add", "Nothing", {},
            [&](int&, std::size_t) {
                ImGui::TextUnformatted("Point");
                summary_colour = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                h.mark("summary");
            },
            [&](int&, std::size_t) { return ui::Edit{}; }, [] { return 0; });
    });
    h.step(3);
    const auto same = [](ImVec4 a, ImVec4 b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; };
    CHECK(same(summary_colour, fjell::theme::text_secondary()));

    const ImVec2 min = h.rect_min("summary");
    ImGui::GetIO().AddMousePosEvent(min.x + 2.0f, min.y + 2.0f);
    h.step(2);
    CHECK(same(summary_colour, fjell::theme::text()));
}
