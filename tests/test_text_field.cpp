#include "imgui_harness.hpp"
#include "ui/kit/text_field.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using fjell::test::ImGuiHarness;
using fjell::ui::TextField;

namespace {

// A name field over `name` with a button beside it to click away to, the
// way the Properties panel draws a node's name. Every commit is applied to
// `name` and counted.
struct NameField {
    ImGuiHarness h;
    TextField field;
    std::string name{"Crate"};
    int commits{0};
    int changed_frames{0};

    NameField() {
        h.set_ui([this] {
            auto edit = field.draw("##Name", name);
            h.mark("field");
            if (edit.changed) ++changed_frames;
            if (edit.committed) {
                name = field.text();
                ++commits;
            }
            ImGui::Button("Elsewhere");
            h.mark("elsewhere");
        });
        h.step(2);
    }

    // Clears the field's text: select all, then delete.
    void clear() {
        auto& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        h.press(ImGuiKey_A);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        h.step();
        h.press(ImGuiKey_Delete);
    }
};

} // namespace

TEST_CASE("TextField shows the value it is drawn with", "[ui][text_field]") {
    NameField f;
    CHECK(f.field.text() == "Crate");
    f.name = "Barrel";
    f.h.step();
    CHECK(f.field.text() == "Barrel");
}

TEST_CASE("TextField commits once, on Enter, not per keystroke", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.clear();
    f.h.type("Crate2");
    CHECK(f.changed_frames > 0);
    CHECK(f.commits == 0);
    CHECK(f.name == "Crate");

    f.h.press(ImGuiKey_Enter);
    CHECK(f.commits == 1);
    CHECK(f.name == "Crate2");
}

TEST_CASE("TextField commits when it loses focus", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.clear();
    f.h.type("Lamp");
    f.h.click("elsewhere");
    CHECK(f.commits == 1);
    CHECK(f.name == "Lamp");
}

TEST_CASE("TextField restores the old text on Escape and commits nothing", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.clear();
    f.h.type("Oops");
    f.h.press(ImGuiKey_Escape);
    CHECK(f.commits == 0);
    CHECK(f.name == "Crate");
    f.h.step();
    CHECK(f.field.text() == "Crate");
}

TEST_CASE("TextField commits nothing when the text ends where it started", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.h.press(ImGuiKey_Enter);
    CHECK(f.commits == 0);
}

TEST_CASE("TextField keeps typed text when the value changes underneath it", "[ui][text_field]") {
    NameField f;
    f.h.click("field");
    f.clear();
    f.h.type("Crate2");
    f.name = "Renamed elsewhere";
    f.h.step();
    CHECK(f.field.text() == "Crate2");
    f.h.press(ImGuiKey_Enter);
    CHECK(f.name == "Crate2");
}
