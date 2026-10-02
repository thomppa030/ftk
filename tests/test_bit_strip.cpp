#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/bit_strip.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;

namespace {

// A strip of four: one set, one inherited, one that can be set and one that
// cannot.
struct Fixture {
    ImGuiHarness h;
    std::array<ui::Bit, 4> bits{ui::Bit{.state = ui::BitState::on}, ui::Bit{.state = ui::BitState::inherited},
                                ui::Bit{.state = ui::BitState::off}, ui::Bit{.state = ui::BitState::unavailable}};
    int clicked{-1};
    ImVec2 origin{};

    Fixture() {
        h.set_ui([this] {
            origin = ImGui::GetCursorScreenPos();
            if (const auto bit = ui::bit_strip("##bits", bits)) clicked = static_cast<int>(*bit);
        });
        h.step(3);
    }

    // The centre of square `i`: 30 px wide, 4 px apart, a frame tall.
    [[nodiscard]] ImVec2 square(int i) const {
        return {origin.x + 15.0f + 34.0f * static_cast<float>(i), origin.y + ImGui::GetFrameHeight() * 0.5f};
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

TEST_CASE("A bit that can be set or cleared reports its click", "[ui][kit]") {
    Fixture f;
    click_at(f.h, f.square(0));
    CHECK(f.clicked == 0);
    click_at(f.h, f.square(2));
    CHECK(f.clicked == 2);
}

TEST_CASE("An inherited or unnamed bit does nothing when clicked", "[ui][kit]") {
    Fixture f;
    click_at(f.h, f.square(1));
    CHECK(f.clicked == -1);
    click_at(f.h, f.square(3));
    CHECK(f.clicked == -1);
}
