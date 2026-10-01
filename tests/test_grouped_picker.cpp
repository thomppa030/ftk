#include "ftk/ui/kit/grouped_picker.hpp"
#include "imgui_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using ftk::test::ImGuiHarness;
namespace picker = ftk::ui::grouped_picker;

namespace {

// A picker of three items, the first of which can't be picked, open under
// a button the way a field opens it.
struct OpenPicker {
    ImGuiHarness h;
    std::vector<picker::Group> groups;
    std::string picked;
    int picks = 0;
    bool open_next = true;  // opened from inside the UI, where its caller's ids are
    bool is_open = false;

    OpenPicker() {
        picker::Group group{.label = "Physics"};
        group.items.push_back({.value = "mesh", .label = "Mesh", .enabled = false,
                               .disabled_reason = "Crate already has a Mesh"});
        group.items.push_back({.value = "rigidbody", .label = "Rigidbody"});
        group.items.push_back({.value = "collider", .label = "Collider"});
        groups.push_back(std::move(group));
        h.set_ui([this] {
            ImGui::Button("Add component");
            if (std::exchange(open_next, false)) picker::open("##add");
            std::string value;
            if (picker::draw("##add", groups, {}, value)) {
                picked = value;
                ++picks;
            }
            is_open = picker::is_open("##add");
        });
        h.step(3);
    }
    ~OpenPicker() { picker::close(); }
};

} // namespace

TEST_CASE("Enter in the picker takes the first item that can be picked", "[ui][picker]") {
    OpenPicker p;
    p.h.press(ImGuiKey_Enter);
    CHECK(p.picks == 1);
    CHECK(p.picked == "rigidbody");
    CHECK_FALSE(p.is_open);
}

TEST_CASE("The arrow keys move through the pickable items and stop at the ends", "[ui][picker]") {
    OpenPicker p;
    p.h.press(ImGuiKey_DownArrow);
    p.h.press(ImGuiKey_DownArrow);  // already on the last
    p.h.press(ImGuiKey_Enter);
    CHECK(p.picked == "collider");

    p.open_next = true;
    p.h.step(3);
    p.h.press(ImGuiKey_UpArrow);    // never onto the disabled one above
    p.h.press(ImGuiKey_Enter);
    CHECK(p.picked == "rigidbody");
}

TEST_CASE("Two pickers with the same id under different ids of their own are two pickers",
          "[ui][picker]") {
    // As two fields of one kind in rows that push their own ids: opening one
    // must not be closed again by the other drawing.
    ImGuiHarness h;
    std::vector<picker::Group> groups(1);
    groups[0].items.push_back({.value = "a", .label = "A"});
    int open_row = 1;
    bool open_now[2] = {false, false};
    std::string picked[2];
    h.set_ui([&] {
        for (int row = 0; row < 2; ++row) {
            ImGui::PushID(row);
            ImGui::Button("Field");
            if (open_row == row) {
                picker::open("##ref");
                open_row = -1;
            }
            std::string value;
            if (picker::draw("##ref", groups, {}, value)) picked[row] = value;
            open_now[row] = picker::is_open("##ref");
            ImGui::PopID();
        }
    });
    h.step(3);
    CHECK_FALSE(open_now[0]);
    CHECK(open_now[1]);
    h.press(ImGuiKey_Enter);
    CHECK(picked[1] == "a");
    CHECK(picked[0].empty());
    picker::close();
}
