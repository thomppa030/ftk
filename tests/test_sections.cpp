#include "kit_harness.hpp"

#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/inset_group.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/section.hpp"
#include "ftk/ui/kit/tabs.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace theme = ftk::theme;

TEST_CASE("A foldable section opens and closes and remembers", "[ui][kit]") {
    KitHarness h;
    bool open = false;
    h.set_ui([&] {
        open = ui::section_foldable("Weather", nullptr, true);
        h.mark("heading");
    });
    h.step(2);
    CHECK(open);
    h.click("heading");
    CHECK_FALSE(open);
    h.step(3);
    CHECK_FALSE(open);
    h.click("heading");
    CHECK(open);
}

TEST_CASE("A foldable sub-heading starts the way it is asked to", "[ui][kit]") {
    KitHarness h;
    bool open = true;
    h.set_ui([&] {
        open = ui::subheading_foldable("Lightning", false);
        h.mark("heading");
    });
    h.step(2);
    CHECK_FALSE(open);
    h.click("heading");
    CHECK(open);
}

TEST_CASE("An inset group folds from its heading and keeps its contents inside it", "[ui][kit]") {
    KitHarness h;
    bool drawn = false;
    float window_right = 0.0f;
    float frame = 0.0f;
    h.set_ui([&] {
        window_right = ui::cursor().x + ui::space_left().x;
        frame = ui::frame_height();
        drawn = false;
        if (auto box = ui::InsetGroup("##box", "Material", nullptr, {1, 1, 1, 1})) {
            drawn = true;
            ui::next_width(ui::FILL);
            float v = 0.5f;
            (void)ui::drag("##v", v);
            h.mark("field");
        }
    });
    h.step(2);
    REQUIRE(drawn);
    // The field stops at the box's padding, not at the window's edge.
    CHECK(h.rect("field").max.x < window_right - 4.0f);
    // The heading is the box's first line: clicking it folds the box.
    const glm::vec2 field_min = h.rect("field").min;
    h.click_at({field_min.x + 40.0f, field_min.y - frame * 0.5f - 2.0f});
    h.step();
    CHECK_FALSE(drawn);
}

TEST_CASE("A named box's remove icon removes without folding it", "[ui][kit]") {
    KitHarness h;
    bool remove = false;
    bool open = false;
    float right = 0.0f;
    float top = 0.0f;
    float side = 0.0f;
    h.set_ui([&] {
        top = ui::cursor().y;
        right = ui::cursor().x + ui::space_left().x;
        side = ui::frame_height();
        auto box = ui::InsetGroup("##jump", {.text = "jump", .named = true, .remove = &remove});
        open = static_cast<bool>(box);
    });
    h.step(2);
    // The remove icon: a frame-height square at the heading's right end.
    h.click_at({right - side * 0.5f - 4.0f, top + 3.0f + side * 0.5f});
    h.step();
    CHECK(remove);
    CHECK(open);
}

TEST_CASE("A tab strip reports only the user's picks, not a selection the caller moved", "[ui][kit]") {
    KitHarness h;
    const std::string names[] = {"embers", "smoke", "sparks"};
    int selected = 0;
    int picks = 0;
    h.set_ui([&] {
        const auto tabs = ui::tab_strip("##emitters", {.names = names, .selected = selected, .add_label = "Add emitter"});
        if (tabs.selected) {
            ++picks;
            selected = *tabs.selected;
        }
    });
    h.step(3);
    // An emitter added or removed moves the selection from outside.
    selected = 2;
    h.step(4);
    CHECK(picks == 0);
    CHECK(selected == 2);
}

TEST_CASE("Document tabs say which is in front, come forward when asked and close", "[ui][kit]") {
    KitHarness h;
    std::array<bool, 3> front{};
    std::array<bool, 3> closed{};
    int select = -1;
    h.set_ui([&] {
        if (auto tabs = ui::DocumentTabs("##docs")) {
            static constexpr const char* NAMES[] = {"Scene", "Rock", "Grass"};
            for (int i = 0; i < 3; ++i) {
                ui::IdScope tab_id(i);
                const auto tab = ui::document_tab(
                    {.label = NAMES[i], .closable = i != 0, .unsaved = i == 1, .select = select == i});
                front[static_cast<std::size_t>(i)] = tab.front;
                closed[static_cast<std::size_t>(i)] = closed[static_cast<std::size_t>(i)] || tab.closed;
                h.mark(NAMES[i]);
            }
        }
        select = -1;
    });
    h.step(2);
    CHECK(front == std::array<bool, 3>{true, false, false});
    select = 2;
    h.step(2);
    CHECK(front == std::array<bool, 3>{false, false, true});
    h.click("Rock");
    h.step();
    CHECK(front == std::array<bool, 3>{false, true, false});

    // The × sits at the right end of the tab in front.
    const auto rock = h.rect("Rock");
    h.click_at({rock.max.x - rock.height() * 0.5f, rock.centre().y});
    CHECK(closed[1]);
    CHECK_FALSE(closed[0]);
    CHECK_FALSE(closed[2]);
}

TEST_CASE("Document tabs of one name are told apart by what follows ##, which isn't shown", "[ui][kit]") {
    KitHarness h;
    std::array<bool, 2> front{};
    h.set_ui([&] {
        if (auto tabs = ui::DocumentTabs("##docs")) {
            front[0] = ui::document_tab({.label = "main.cpp##game", .icon = ui::icon::file, .hue = theme::Hue::slate}).front;
            h.mark("game");
            front[1] = ui::document_tab({.label = "main.cpp##tools", .icon = ui::icon::file}).front;
            h.mark("tools");
        }
    });
    h.step(2);
    std::size_t named = 0;
    for (const ui::core::SemanticNode& node : h.ui().semantics()) named += node.role == ui::core::Role::Tab && node.label == "main.cpp";
    CHECK(named == 2);
    h.click("tools");
    h.step();
    CHECK(front == std::array<bool, 2>{false, true});
}

TEST_CASE("The context bar takes the top of the work area, as tall as a tab and its room", "[ui][kit]") {
    KitHarness h;
    ui::WorkArea area{};
    float line = 0.0f;
    h.set_ui([&] {
        area = ui::work_area();
        line = ui::line_height();
        if (auto bar = ui::ContextTabBar("##context")) {
            if (auto tabs = ui::DocumentTabs("##documents")) {
                (void)ui::document_tab({.label = "Scene", .closable = false});
                h.mark("scene");
            }
        }
    });
    h.step(3);
    // A tab is a line of text with eight above and below it, and the bar
    // two more each way.
    CHECK(h.rect("scene").height() == line + 16.0f);
    CHECK(area.pos.y == line + 20.0f);
    CHECK(area.pos.y + area.size.y == h.size().y);
}
