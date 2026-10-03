#include "kit_harness.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/grouped_picker.hpp"
#include "ftk/ui/kit/scope.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace picker = ftk::ui::grouped_picker;

namespace {

// A picker of three items, the first of which can't be picked, open under
// a button the way a field opens it.
struct OpenPicker {
    KitHarness h;
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
            ui::button("Add component");
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

TEST_CASE("Each row of the picker shows its label inside the row", "[ui][picker]") {
    // A row is a selectable across the list with what it shows after it on
    // its line, from the row's start: the label is drawn inside the row,
    // not past its end.
    OpenPicker p;
    std::vector<ui::core::Rect> rows;
    for (const ui::core::SemanticNode& node : p.h.ui().semantics()) {
        if (node.role == ui::core::Role::ListItem) rows.push_back(node.rect);
    }
    REQUIRE(rows.size() == 3);
    std::vector<int> glyphs(rows.size(), 0);
    for (const ui::core::RootOutput& root : p.h.output().roots) {
        if (root.draw == nullptr) continue;
        for (const ui::core::Primitive& primitive : root.draw->primitives) {
            if (primitive.kind != ui::core::PrimitiveKind::Glyph) continue;
            const glm::vec2 centre = (primitive.min + primitive.max) * 0.5f;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].contains(centre)) ++glyphs[i];
            }
        }
    }
    for (const int count : glyphs) CHECK(count > 0);
}

TEST_CASE("Enter in the picker takes the first item that can be picked", "[ui][picker]") {
    OpenPicker p;
    p.h.press(ui::Key::Enter);
    CHECK(p.picks == 1);
    CHECK(p.picked == "rigidbody");
    CHECK_FALSE(p.is_open);
}

TEST_CASE("The arrow keys move through the pickable items and stop at the ends", "[ui][picker]") {
    OpenPicker p;
    p.h.press(ui::Key::Down);
    p.h.press(ui::Key::Down);  // already on the last
    p.h.press(ui::Key::Enter);
    CHECK(p.picked == "collider");

    p.open_next = true;
    p.h.step(3);
    p.h.press(ui::Key::Up);    // never onto the disabled one above
    p.h.press(ui::Key::Enter);
    CHECK(p.picked == "rigidbody");
}

TEST_CASE("Two pickers with the same id under different ids of their own are two pickers",
          "[ui][picker]") {
    // As two fields of one kind in rows that push their own ids: opening one
    // must not be closed again by the other drawing.
    KitHarness h;
    std::vector<picker::Group> groups(1);
    groups[0].items.push_back({.value = "a", .label = "A"});
    int open_row = 1;
    bool open_now[2] = {false, false};
    std::string picked[2];
    h.set_ui([&] {
        for (int row = 0; row < 2; ++row) {
            const ui::IdScope id(row);
            ui::button("Field");
            if (open_row == row) {
                picker::open("##ref");
                open_row = -1;
            }
            std::string value;
            if (picker::draw("##ref", groups, {}, value)) picked[row] = value;
            open_now[row] = picker::is_open("##ref");
        }
    });
    h.step(3);
    CHECK_FALSE(open_now[0]);
    CHECK(open_now[1]);
    h.press(ui::Key::Enter);
    CHECK(picked[1] == "a");
    CHECK(picked[0].empty());
    picker::close();
}
