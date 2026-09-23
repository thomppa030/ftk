#include "imgui_harness.hpp"
#include "ui/grouped_picker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using fjell::test::ImGuiHarness;
namespace picker = fjell::grouped_picker;

namespace {

// A picker of three items, the first of which can't be picked, open under
// a button the way a field opens it.
struct OpenPicker {
    ImGuiHarness h;
    std::vector<picker::Group> groups;
    std::string picked;
    int picks = 0;

    OpenPicker() {
        picker::Group group{.label = "Physics"};
        group.items.push_back({.value = "mesh", .label = "Mesh", .enabled = false,
                               .disabled_reason = "Crate already has a Mesh"});
        group.items.push_back({.value = "rigidbody", .label = "Rigidbody"});
        group.items.push_back({.value = "collider", .label = "Collider"});
        groups.push_back(std::move(group));
        h.set_ui([this] {
            ImGui::Button("Add component");
            std::string value;
            if (picker::draw("##add", groups, {}, value)) {
                picked = value;
                ++picks;
            }
        });
        picker::open("##add");
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
    CHECK_FALSE(picker::is_open("##add"));
}

TEST_CASE("The arrow keys move through the pickable items and stop at the ends", "[ui][picker]") {
    OpenPicker p;
    p.h.press(ImGuiKey_DownArrow);
    p.h.press(ImGuiKey_DownArrow);  // already on the last
    p.h.press(ImGuiKey_Enter);
    CHECK(p.picked == "collider");

    picker::open("##add");
    p.h.step(3);
    p.h.press(ImGuiKey_UpArrow);    // never onto the disabled one above
    p.h.press(ImGuiKey_Enter);
    CHECK(p.picked == "rigidbody");
}
