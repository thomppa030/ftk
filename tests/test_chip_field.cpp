#include "ftk/test/imgui_harness.hpp"
#include "ftk/ui/kit/chip_field.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using ftk::test::ImGuiHarness;
namespace ui = ftk::ui;

namespace {

// A field holding one chip of its own and one inherited, over a picker of
// three entries and a footer.
struct Fixture {
    ImGuiHarness h;
    std::array<ui::Chip, 2> chips{ui::Chip{.label = "interactable"},
                                  ui::Chip{.label = "inked", .inherited = true, .tooltip = "From Chest"}};
    std::array<ui::ChipOption, 3> options{ui::ChipOption{.label = "interactable", .on = true, .note = "1 node"},
                                          ui::ChipOption{.label = "enemy"}, ui::ChipOption{.label = "inked", .on = true}};
    ui::ChipFooter footer{.label = "Edit marks"};
    ui::ChipFieldEdit last;
    int removed{-1};
    int toggled{-1};
    bool footer_chosen{false};
    ImVec2 origin{};

    Fixture() {
        h.set_ui([this] {
            origin = ImGui::GetCursorScreenPos();
            ImGui::SetNextItemWidth(320.0f);
            last = ui::chip_field("##marks", chips, options, &footer);
            if (last.removed) removed = static_cast<int>(*last.removed);
            if (last.toggled) toggled = static_cast<int>(*last.toggled);
            if (last.footer) footer_chosen = true;
        });
        h.step(3);
    }

    // The centre of the first chip's remove: at its right end, the chip
    // sized by its label in the small face.
    [[nodiscard]] ImVec2 first_remove() const {
        ImGui::PushFont(nullptr, ftk::theme::SMALL_TEXT);
        const float label = ImGui::CalcTextSize("interactable").x;
        ImGui::PopFont();
        const float chip_right = origin.x + 4.0f + 7.0f + label + 4.0f + 16.0f + 1.0f;
        return {chip_right - 1.0f - 8.0f, origin.y + ImGui::GetFrameHeight() * 0.5f};
    }
    // The field's empty end, left of the chevron.
    [[nodiscard]] ImVec2 field_end() const { return {origin.x + 250.0f, origin.y + ImGui::GetFrameHeight() * 0.5f}; }
    // The centre of picker entry `i`, the footer one past the options.
    [[nodiscard]] ImVec2 entry(int i) const {
        const float top = origin.y + ImGui::GetFrameHeight() + 2.0f + 4.0f;
        const float y = i < 3 ? top + 26.0f * static_cast<float>(i) + 13.0f : top + 26.0f * 3.0f + 9.0f + 13.0f;
        return {origin.x + 60.0f, y};
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

TEST_CASE("A chip's remove takes it off, and opens nothing", "[ui][kit]") {
    Fixture f;
    click_at(f.h, f.first_remove());
    CHECK(f.removed == 0);
    // A click on the field's empty end opens the picker; its first entry is
    // then under the mouse to be toggled, which a closed picker would not do.
    click_at(f.h, f.entry(1));
    CHECK(f.toggled == -1);
}

TEST_CASE("The picker stays open while entries are toggled, and its footer closes it", "[ui][kit]") {
    Fixture f;
    click_at(f.h, f.field_end());
    CHECK(f.removed == -1);
    click_at(f.h, f.entry(1));
    CHECK(f.toggled == 1);
    f.toggled = -1;
    click_at(f.h, f.entry(2));
    CHECK(f.toggled == 2);
    click_at(f.h, f.entry(3));
    CHECK(f.footer_chosen);
    f.toggled = -1;
    click_at(f.h, f.entry(0));
    CHECK(f.toggled == -1);
}
