#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/rail.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;

namespace {

// Two steps with items between and after them, the first item shown at a
// second place too.
struct Fixture {
    ImGuiHarness h;
    std::array<ui::RailEntry, 5> entries{ui::RailEntry{.label = "Load"},
                                         ui::RailEntry{.label = "Resize", .item = 0},
                                         ui::RailEntry{.label = "Save"},
                                         ui::RailEntry{.label = "Upload", .item = 1},
                                         ui::RailEntry{.label = "Resize", .item = 0, .note = "again"}};
    int clicked{-1};
    ImVec2 origin{};

    Fixture() {
        h.set_ui([this] {
            origin = ImGui::GetCursorScreenPos();
            if (const auto item = ui::rail("##steps", entries, ftk::theme::Hue::blue)) {
                clicked = static_cast<int>(*item);
            }
        });
        h.step(3);
    }

    // The middle of line `i`, a little in from the left of its text: steps
    // are 24 px tall, items 26.
    [[nodiscard]] ImVec2 line(int i) const {
        float y = origin.y;
        for (int k = 0; k < i; ++k) y += entries[static_cast<std::size_t>(k)].item ? 26.0f : 24.0f;
        y += entries[static_cast<std::size_t>(i)].item ? 13.0f : 12.0f;
        return {origin.x + 40.0f, y};
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

} // namespace

TEST_CASE("Clicking a rail item reports which item it is, wherever it shows", "[ui][kit]") {
    Fixture f;
    click_at(f.h, f.line(3));
    CHECK(f.clicked == 1);
    click_at(f.h, f.line(1));
    CHECK(f.clicked == 0);
    f.clicked = -1;
    click_at(f.h, f.line(4));
    CHECK(f.clicked == 0);
}

TEST_CASE("A rail's steps do nothing when clicked", "[ui][kit]") {
    Fixture f;
    click_at(f.h, f.line(0));
    click_at(f.h, f.line(2));
    CHECK(f.clicked == -1);
}
