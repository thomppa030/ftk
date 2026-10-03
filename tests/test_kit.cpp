#include "kit_harness.hpp"

#include "ftk/ui/draw_list.hpp"
#include "ftk/ui/kit/asset_kind.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/key_cap.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/loading.hpp"
#include "ftk/ui/kit/overlay.hpp"
#include "ftk/ui/kit/region.hpp"
#include "ftk/ui/kit/row.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

TEST_CASE("Every button kind reports its click", "[ui][kit]") {
    const ui::ButtonKind kinds[] = {ui::ButtonKind::Secondary, ui::ButtonKind::Primary,
                                    ui::ButtonKind::Ghost, ui::ButtonKind::Danger,
                                    ui::ButtonKind::GhostDanger};
    for (auto kind : kinds) {
        KitHarness h;
        int clicks = 0;
        h.set_ui([&] {
            if (ui::button("Save", kind)) ++clicks;
            h.mark("button");
        });
        h.step(2);
        h.click("button");
        CHECK(clicks == 1);
    }
}

TEST_CASE("An action leads with its icon and is as tall as any button", "[ui][kit]") {
    KitHarness h;
    int clicks = 0;
    float icon_room = 0.0f;
    h.set_ui([&] {
        ui::button("Create material");
        h.mark("button");
        if (ui::action("+", "Create material")) ++clicks;
        h.mark("action");
        icon_room = ui::text_size("+  ").x;
    });
    h.step(2);
    CHECK(h.rect("action").height() == h.rect("button").height());
    // Wider than the same verb on a plain button by the icon and its gap.
    CHECK(h.rect("action").width() >= h.rect("button").width() + icon_room - 0.5f);
    h.click("action");
    CHECK(clicks == 1);
}

TEST_CASE("A small button is a line of text tall and is clicked", "[ui][kit]") {
    KitHarness h;
    int stops = 0;
    float height = 0.0f;
    h.set_ui([&] {
        if (ui::small_button("Stop")) ++stops;
        h.mark("stop");
        height = ui::item_max().y - ui::item_min().y;
    });
    h.step(2);
    CHECK(height == ui::line_height());
    h.click("stop");
    CHECK(stops == 1);
}

TEST_CASE("A progress bar is the width it is given", "[ui][kit]") {
    KitHarness h;
    float width = 0.0f;
    h.set_ui([&] {
        ui::progress_bar(0.5f, 120.0f, "Halfway");
        width = ui::item_max().x - ui::item_min().x;
    });
    h.step(2);
    CHECK(width == 120.0f);
}

TEST_CASE("An icon button is a frame-height square", "[ui][kit]") {
    KitHarness h;
    int clicks = 0;
    h.set_ui([&] {
        if (ui::icon_button("clear", "x", "Clear")) ++clicks;
        h.mark("icon");
    });
    h.step(2);
    CHECK(h.rect("icon").width() == h.rect("icon").height());
    h.click("icon");
    CHECK(clicks == 1);
}

TEST_CASE("A toggle reports clicks and leaves the state to the caller", "[ui][kit]") {
    KitHarness h;
    bool on = false;
    h.set_ui([&] {
        if (ui::toggle_button("snap", "M", on, "Snap to grid")) on = !on;
        h.mark("toggle");
    });
    h.step(2);
    h.click("toggle");
    CHECK(on);
    h.click("toggle");
    CHECK_FALSE(on);
}

TEST_CASE("An empty state sits in the middle of the space and offers its action", "[ui][kit]") {
    KitHarness h;
    int clicks = 0;
    glm::vec2 area_min{};
    glm::vec2 area_max{};
    h.set_ui([&] {
        area_min = ui::cursor();
        area_max = area_min + ui::space_left();
        if (ui::empty_state("?", "Nothing selected", "Pick an object to edit it here", "Select all")) {
            ++clicks;
        }
        h.mark("action");
    });
    h.step(2);
    const glm::vec2 max = h.rect("action").max;
    CHECK(std::abs(h.rect("action").centre().x - (area_min.x + area_max.x) * 0.5f) <= 1.0f);
    // The block is centred vertically, so its last line is below the middle
    // but nowhere near the bottom.
    CHECK(max.y > (area_min.y + area_max.y) * 0.5f);
    CHECK(max.y < area_max.y - 100.0f);
    h.click("action");
    CHECK(clicks == 1);
}

TEST_CASE("An asset's kind comes from its extension, by domain", "[ui][kit]") {
    using ftk::theme::Hue;
    CHECK(ui::asset_kind(".png").hue == Hue::blue);
    CHECK(std::string(ui::asset_kind(".png").noun) == "texture");
    CHECK(ui::asset_kind(".ttf").hue == Hue::lilac);
    CHECK(ui::asset_kind(".wav").hue == Hue::orange);
    CHECK(std::string(ui::asset_kind(".xyz").noun) == "file");
}

TEST_CASE("A program's own file types show as it registered them", "[ui][kit]") {
    using ftk::theme::Hue;
    ui::register_asset_kind(".kittest", {"kit test", ui::icon::file, Hue::slate});
    CHECK(std::string(ui::asset_kind(".kittest").noun) == "kit test");
    CHECK(ui::asset_kind(".kittest").hue == Hue::slate);
}

TEST_CASE("A disabled icon button still shows its tooltip", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        {
            ui::Disabled off;
            ui::icon_button("clear", "x", "Nothing to clear");
        }
        h.mark("button");
    });
    h.step(2);
    // Past the tooltip delay.
    h.hover("button");
    CHECK(h.tooltip().has_value());
}

TEST_CASE("A callout wraps its text, reports its action, and the layout goes on below it", "[ui][kit]") {
    KitHarness h;
    int clicks = 0;
    float line = 0.0f;
    float frame = 0.0f;
    h.set_ui([&] {
        line = ui::line_height() + ui::item_spacing().y;
        frame = ui::frame_height();
        if (auto narrow = ui::Region("##narrow", {220.0f, 400.0f})) {
            if (ui::callout(ui::Severity::Error, "sea.fjsl failed to compile",
                            "Line 42: 'foam_bias' : undeclared identifier. It draws with the last "
                            "version that compiled.",
                            "+", "Open sea.fjsl")) {
                ++clicks;
            }
            h.mark("box");
            ui::button("after");
            h.mark("after");
        }
    });
    h.step(3);
    // The explanation wraps inside the 220 px window onto several lines:
    // the box is taller than a title, three lines and the button.
    CHECK(h.rect("box").height() > line * 4.0f + frame);
    CHECK(h.rect("after").min.y >= h.rect("box").max.y);
    // The action is the last thing in the box, under the text: press and
    // let go on it.
    const glm::vec2 action{h.rect("box").min.x + 60.0f, h.rect("box").max.y - 8.0f - frame * 0.5f};
    h.drag(action, action);
    CHECK(clicks == 1);
}

TEST_CASE("A tool button is a tool-sized square and reports its click", "[ui][kit]") {
    KitHarness h;
    bool on = false;
    h.set_ui([&] {
        if (ui::tool_button("raise", "R", on, "Raise")) on = !on;
        h.mark("tool");
    });
    h.step(2);
    CHECK(h.rect("tool").width() == ftk::theme::TOOL_BUTTON);
    CHECK(h.rect("tool").height() == ftk::theme::TOOL_BUTTON);
    h.click("tool");
    CHECK(on);
}

TEST_CASE("A mode button is as wide as mode_button_width says, to be placed by it", "[ui][kit]") {
    KitHarness h;
    h.set_ui([&] {
        (void)ui::mode_button("##view", ui::icon::search, "DDGI indirect", true);
        h.mark("view");
    });
    h.step(2);
    const float drawn = h.rect("view").width();
    CHECK(drawn == ui::mode_button_width(ui::icon::search, "DDGI indirect"));
    CHECK(drawn > ui::mode_button_width(ui::icon::search, "Lit"));
}

TEST_CASE("A marked row's label makes room for the dot, marked or not, and only that row", "[ui][kit]") {
    KitHarness h;
    float plain = 0.0f;
    float set_here = 0.0f;
    float not_set = 0.0f;
    float after_skipped = 0.0f;
    h.set_ui([&] {
        if (auto t = ui::PropertyTable("##rows")) {
            // In the value column the last item is still the row's label.
            ui::row("Plain", [&] { plain = ui::item_min().x; });
            ui::mark_next_row(true);
            ui::row("Set here", [&] { set_here = ui::item_min().x; });
            ui::mark_next_row(false);
            ui::row("Not set", [&] { not_set = ui::item_min().x; });
            {
                // A search that leaves the marked row out doesn't hand its
                // mark to the next row.
                ui::RowFilter filter("Next");
                ui::mark_next_row(true);
                ui::row("Skipped", [] {});
                ui::row("Next", [&] { after_skipped = ui::item_min().x; });
            }
        }
    });
    h.step(2);
    CHECK(set_here > plain);
    CHECK(not_set == set_here);
    CHECK(after_skipped == plain);
}

TEST_CASE("An on/off switch flips its value, each flip a finished edit", "[ui][kit]") {
    KitHarness h;
    bool on = true;
    int commits = 0;
    h.set_ui([&] {
        if (ui::on_off("##switch", on).committed) ++commits;
        h.mark("switch");
    });
    h.step(2);
    h.click("switch");
    CHECK_FALSE(on);
    h.click("switch");
    CHECK(on);
    CHECK(commits == 2);
}

TEST_CASE("A key cap listens from its name and lists from its chevron", "[ui][kit]") {
    KitHarness h;
    int listens = 0;
    int lists = 0;
    glm::vec2 start{};
    float frame = 0.0f;
    float short_end = 0.0f;
    float long_end = 0.0f;
    float long_start = 0.0f;
    h.set_ui([&] {
        start = ui::cursor();
        frame = ui::frame_height();
        const auto cap = ui::key_cap({.id = "##w", .device_icon = ui::icon::file, .label = "W"});
        h.mark("chevron");  // the last item: the chevron
        short_end = ui::item_max().x;
        if (cap.listen) ++listens;
        if (cap.list) ++lists;
        long_start = ui::cursor().x;
        (void)ui::key_cap({.id = "##shift", .device_icon = ui::icon::file, .label = "Left Shift"});
        long_end = ui::item_max().x;
    });
    h.step(2);
    h.click("chevron");
    CHECK(lists == 1);
    CHECK(listens == 0);
    h.click_at({start.x + 6.0f, start.y + frame * 0.5f});
    h.step();
    CHECK(listens == 1);
    CHECK(long_end - long_start > short_end - start.x);
}

TEST_CASE("A viewport pill sits inside the image's corner it is placed in", "[ui][kit]") {
    KitHarness h;
    const glm::vec2 image_min{20.0f, 30.0f};
    const glm::vec2 image_max{420.0f, 330.0f};
    glm::vec2 left_min{};
    float right_edge = 0.0f;
    h.set_ui([&] {
        {
            ui::ViewportPill pill("##left", image_min, image_max, ui::PillPlace::TopLeft);
            (void)ui::icon_button("##grid", ui::icon::folder, "Grid");
            left_min = ui::item_min();
        }
        {
            ui::ViewportPill pill("##right", image_min, image_max, ui::PillPlace::TopRight);
            (void)ui::icon_button("##a", ui::icon::folder, "A");
            ui::same_line();
            (void)ui::icon_button("##b", ui::icon::folder, "B");
            right_edge = ui::item_max().x;
        }
    });
    // The right one learns its width in the first frame and is placed by it after.
    h.step(3);
    CHECK(left_min.x > image_min.x);
    CHECK(left_min.y > image_min.y);
    CHECK(right_edge < image_max.x);
    CHECK(right_edge > image_max.x - 16.0f);
}

TEST_CASE("A viewport window puts its picture at its very corner", "[ui][kit]") {
    KitHarness h;
    glm::vec2 window_pos{};
    glm::vec2 first{};
    h.set_ui([&] {
        ui::next_window_place({50.0f, 60.0f}, {200.0f, 150.0f});
        if (auto window = ui::ViewportWindow("##viewport", nullptr, {.title_bar = false})) {
            window_pos = ui::window_pos();
            first = ui::cursor();
        }
    });
    h.step(2);
    CHECK(first.x == window_pos.x);
    CHECK(first.y == window_pos.y);
}

TEST_CASE("A corner note hangs from its corner, and a key makes its line taller", "[ui][kit]") {
    KitHarness h;
    glm::vec2 words{};
    glm::vec2 with_key{};
    glm::vec2 longer{};
    h.set_ui([&] {
        const ui::DrawList dl = ui::DrawList::window();
        const ftk::ui::OverlayPiece plain[] = {{"releases input"}};
        const ftk::ui::OverlayPiece keyed[] = {{"Esc", true}, {"releases input"}};
        const ftk::ui::OverlayPiece more[] = {{"releases input, and more"}};
        words = ui::corner_note(dl, {400.0f, 300.0f}, ui::Corner::BottomRight,
                                {.line = plain, .ink = ftk::theme::text_secondary()});
        with_key = ui::corner_note(dl, {0.0f, 0.0f}, ui::Corner::TopLeft,
                                   {.line = keyed, .ink = ftk::theme::text_secondary()});
        longer = ui::corner_note(dl, {0.0f, 0.0f}, ui::Corner::TopLeft,
                                 {.line = more, .ink = ftk::theme::text_secondary()});
    });
    h.step(2);
    CHECK(with_key.y > words.y);
    CHECK(with_key.x > words.x);
    CHECK(longer.x > words.x);
    CHECK(longer.y == words.y);
}

TEST_CASE("A tag after a field is as wide as tag_width says and stays on the field's line", "[ui][kit]") {
    KitHarness h;
    glm::vec2 field_min{};
    glm::vec2 tag_min{};
    glm::vec2 tag_size{};
    float frame = 0.0f;
    h.set_ui([&] {
        frame = ui::frame_height();
        bool on = true;
        (void)ui::checkbox("##switch", on);
        field_min = ui::item_min();
        ui::same_line();
        ui::tag("+1 shader", "Compiled in", "2 shaders");
        tag_min = ui::item_min();
        tag_size = ui::item_max() - ui::item_min();
    });
    h.step(2);
    CHECK(tag_size.x == ui::tag_width("+1 shader"));
    CHECK(tag_size.x > ui::tag_width("+1"));
    CHECK(tag_size.y == frame);
    CHECK(tag_min.y == field_min.y);
    CHECK(tag_min.x > field_min.x);
}

TEST_CASE("An inherited row lines its value up with the others and resets only what is set here", "[ui][kit]") {
    KitHarness h;
    float set_value = 0.5f;
    float inherited_value = 0.25f;
    float set_right = 0.0f;
    float inherited_right = 0.0f;
    float plain_right = 0.0f;
    float frame = 0.0f;
    int resets = 0;
    h.set_ui([&] {
        frame = ui::frame_height();
        if (auto t = ui::PropertyTable("##values")) {
            if (ui::inherited_row("Roughness", true, "Use the parent's value", [&] {
                    (void)ui::drag("##set", set_value);
                    set_right = ui::item_max().x;
                })) {
                ++resets;
            }
            h.mark("reset");
            if (ui::inherited_row("Metallic", false, "Use the parent's value", [&] {
                    (void)ui::drag("##inherited", inherited_value);
                    inherited_right = ui::item_max().x;
                })) {
                ++resets;
            }
            ui::row("Plain", [&] {
                (void)ui::drag("##plain", inherited_value);
                plain_right = ui::item_max().x;
            });
        }
    });
    h.step(2);
    CHECK(set_right == inherited_right);
    // The reset's room is left after the value, set here or not.
    CHECK(set_right < plain_right - frame);
    h.click("reset");
    CHECK(resets == 1);
}

TEST_CASE("A status line with an action keeps its text short of the button and reports its click", "[ui][kit]") {
    KitHarness h;
    glm::vec2 start{};
    float right = 0.0f;
    float after = 0.0f;
    float frame = 0.0f;
    int clicks = 0;
    h.set_ui([&] {
        start = ui::cursor();
        right = start.x + ui::space_left().x;
        frame = ui::frame_height();
        if (ui::status(ui::Severity::Warning,
                       "leaf_colour is no longer in foliage.fjsl, and this line is long enough to wrap",
                       ui::icon::remove, "Remove")) {
            ++clicks;
        }
        after = ui::cursor().y;
    });
    h.step(2);
    // What follows starts below the line, button and wrapped text alike.
    CHECK(after >= start.y + frame);
    // The button sits at the end of the line's first row.
    h.click_at({right - 4.0f, start.y + frame * 0.5f});
    CHECK(clicks == 1);
    // The text does not answer for it.
    h.click_at({start.x + 30.0f, start.y + frame * 0.5f});
    CHECK(clicks == 1);
}
