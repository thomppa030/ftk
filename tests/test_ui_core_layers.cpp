#include "ftk/test/ui_harness.hpp"
#include "ftk/ui/core/context.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <string>
#include <vector>

namespace core = ftk::ui::core;
using core::PointerEvent;
using core::PointerType;
using ftk::test::UiHarness;
using ftk::ui::Id;
using ftk::ui::Key;

namespace {

constexpr ftk::ui::Color PLATE{0.2f, 0.2f, 0.2f, 1.0f};

// A widget at the cursor, marked under its label.
core::Interaction widget(UiHarness& h, const char* label, glm::vec2 size = {100.0f, 24.0f}) {
    core::Context& ui = h.ui();
    const core::Interaction in = ui.interact(ui.id(label), ui.layout().next(size));
    h.mark(label);
    return in;
}

// The panel being drawn, painted, so it can be found in the frame's shapes.
void plate(core::Context& ui, ftk::ui::Color colour = PLATE) {
    const core::Rect r = ui.panel_rect();
    ui.draw_list().rect_filled(r.min, r.max, colour);
}

bool drawn(const UiHarness& h, ftk::ui::Color colour) {
    for (const core::Primitive& p : h.output().roots[0].draw->primitives) {
        if (p.colour == core::pack_colour(colour)) return true;
    }
    return false;
}

} // namespace

TEST_CASE("A panel brought to the front takes the pointer where it overlaps the others", "[ui-core][layers]") {
    UiHarness h;
    bool raise = false;
    bool in_first = false;
    bool in_second = false;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        ui.begin_panel("first", {.min = {0.0f, 0.0f}, .max = {200.0f, 200.0f}}, {.raise = false});
        in_first = widget(h, "a", {150.0f, 150.0f}).hovered;
        ui.end_panel();
        ui.begin_panel("second", {.min = {50.0f, 50.0f}, .max = {250.0f, 250.0f}}, {.raise = false});
        in_second = widget(h, "b", {150.0f, 150.0f}).hovered;
        ui.end_panel();
        if (raise) ui.bring_to_front(ui.id("first"));
        raise = false;
    });
    h.step();
    h.move_to({100.0f, 100.0f});
    h.step();
    CHECK(in_second);
    CHECK_FALSE(in_first);

    raise = true;
    h.step(2);
    CHECK(in_first);
    CHECK_FALSE(in_second);
}

TEST_CASE("A dialog opened as a popup blocks what is beneath and stays through a press outside", "[ui-core][layers]") {
    UiHarness h;
    const Id dialog = 0xD1A1;
    int behind = 0;
    bool shown = false;
    std::vector<core::Rect> rects;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        behind += widget(h, "behind").clicked ? 1 : 0;
        shown = ui.begin_modal_popup(dialog, {.fit = true, .min_size = {200.0f, 60.0f}});
        if (shown) {
            rects.push_back(ui.panel_rect());
            (void)widget(h, "inside", {150.0f, 24.0f});
            ui.end_popup();
        }
    });
    h.step();
    h.ui().open_popup(dialog, {300.0f, 200.0f});
    h.step(3);
    REQUIRE(shown);
    // Before what it holds is measured it is as big as it is to be at least.
    REQUIRE(rects.size() >= 2);
    CHECK(rects[0].size() == glm::vec2{200.0f, 60.0f});
    CHECK(rects.back().min == glm::vec2{300.0f, 200.0f});
    h.click("behind");
    CHECK(behind == 0);
    CHECK(shown);
    CHECK(h.ui().popup_open(dialog));
}

TEST_CASE("A popup opens at the pointer, fits what it holds, and closes on a choice, outside or Escape", "[ui-core][layers]") {
    UiHarness h;
    bool draw_popup = true;
    int deleted = 0;
    core::Rect popup{};
    Id menu = 0;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        menu = ui.id("##menu");
        if (widget(h, "Open").clicked) ui.open_popup(menu);
        (void)widget(h, "beneath", {300.0f, 200.0f});
        if (draw_popup && ui.begin_popup(menu)) {
            popup = ui.panel_rect();
            plate(ui);
            if (widget(h, "Delete").clicked) {
                ++deleted;
                ui.close_popup(menu);
            }
            ui.end_popup();
        }
    });
    h.step();
    h.click("Open");
    CHECK(h.ui().popup_open(menu));
    CHECK(h.ui().any_popup_open());
    // The first frame has nothing to fit, so it shows from the next.
    CHECK_FALSE(drawn(h, PLATE));
    h.step();
    CHECK(drawn(h, PLATE));
    CHECK(popup.min == h.rect("Open").centre());
    CHECK(popup.size() == glm::vec2{116.0f, 40.0f});

    // Over what is beneath it: a press on it doesn't reach that.
    h.click("Delete");
    CHECK(deleted == 1);
    CHECK_FALSE(h.ui().popup_open(menu));

    // A press outside closes it, and Escape does.
    h.click("Open");
    h.step();
    h.click_at({700.0f, 550.0f});
    CHECK_FALSE(h.ui().popup_open(menu));
    h.click("Open");
    h.step();
    h.press(Key::Escape);
    CHECK_FALSE(h.ui().popup_open(menu));

    // Not drawn for a frame: closed.
    h.click("Open");
    h.step();
    draw_popup = false;
    h.step(2);
    CHECK_FALSE(h.ui().popup_open(menu));
}

TEST_CASE("A panel begun again in a frame keeps what it holds, where it is, and settles", "[ui-core][layers]") {
    UiHarness h;
    constexpr ftk::ui::Color OVER{0.4f, 0.4f, 0.4f, 1.0f};
    const Id viewport = core::hash_label("Viewport", 0);
    int first = 0;
    int second = 0;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        ui.begin_panel(viewport, {.min = {300.0f, 100.0f}, .max = {500.0f, 250.0f}});
        plate(ui);
        if (widget(h, "First").clicked) ++first;
        ui.end_panel();
        ui.begin_panel("Hierarchy", {.min = {50.0f, 100.0f}, .max = {250.0f, 250.0f}});
        ui.end_panel();
        // Back in the viewport, by its id, wherever it is begun from; the
        // rectangle given is the one it already has.
        ui.begin_panel(viewport, {.min = {0.0f, 0.0f}, .max = {10.0f, 10.0f}});
        CHECK(ui.panel_rect().min == glm::vec2{300.0f, 100.0f});
        plate(ui, OVER);
        if (widget(h, "Second", {100.0f, 60.0f}).clicked) ++second;
        ui.end_panel();
    });
    h.step(2);
    CHECK(drawn(h, PLATE));
    CHECK(drawn(h, OVER));
    // Its items go on from where the first time left off.
    CHECK(h.rect("Second").min.y > h.rect("First").max.y);
    // Both its widgets take the pointers.
    h.click("First");
    h.click("Second");
    CHECK(first == 1);
    CHECK(second == 1);
    // Measured over both times, it doesn't keep asking for frames.
    h.step(2);
    CHECK(h.output().next.waits_for_input());
}

TEST_CASE("A popup opened near the window's edge is moved inside it", "[ui-core][layers]") {
    UiHarness h;
    core::Rect popup{};
    Id menu = 0;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        menu = ui.id("##menu");
        if (ui.begin_popup(menu)) {
            popup = ui.panel_rect();
            (void)widget(h, "item", {200.0f, 100.0f});
            ui.end_popup();
        }
    });
    h.step();
    h.ui().open_popup(menu, {780.0f, 590.0f});
    h.step(3);
    CHECK(popup.max == glm::vec2{800.0f, 600.0f});
    CHECK(popup.min == glm::vec2{584.0f, 484.0f});
}

TEST_CASE("A dialog blocks the window beneath it, and a popup opened in it is over it", "[ui-core][layers]") {
    UiHarness h;
    int behind = 0;
    int ok = 0;
    int chosen = 0;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        behind += widget(h, "behind").clicked ? 1 : 0;
        ui.begin_modal("Confirm", {.min = {200.0f, 150.0f}, .max = {600.0f, 450.0f}});
        ok += widget(h, "OK").clicked ? 1 : 0;
        const Id choices = ui.id("##choices");
        if (widget(h, "Pick").clicked) ui.open_popup(choices);
        if (ui.begin_popup(choices)) {
            chosen += widget(h, "Choice").clicked ? 1 : 0;
            ui.end_popup();
        }
        ui.end_modal();
    });
    h.step();
    h.click("behind");
    CHECK(behind == 0);
    h.click("OK");
    CHECK(ok == 1);
    h.click("Pick");
    h.step();
    h.click("Choice");
    CHECK(chosen == 1);
}

TEST_CASE("A tooltip shows once the mouse rests for the delay, at a deadline, and takes no pointers", "[ui-core][layers]") {
    UiHarness h;
    int due = 0;
    int hovered = 0;
    const ftk::ui::Color tip{0.9f, 0.8f, 0.1f, 1.0f};
    h.set_ui([&] {
        core::Context& ui = h.ui();
        hovered += widget(h, "Bake").hovered ? 1 : 0;
        if (ui.tooltip_due()) {
            ++due;
            ui.begin_tooltip();
            plate(ui, tip);
            (void)widget(h, "tip text", {150.0f, 16.0f});
            ui.end_tooltip();
        }
    });
    h.step();
    h.move_to(h.rect("Bake").centre());
    const double rested = h.time();
    CHECK(due == 0);
    // The frame after the move, which a frame that took input asks for;
    // then asleep until the delay is up.
    CHECK(h.output().next.now);
    h.step();
    REQUIRE(h.output().next.at.has_value());
    CHECK(*h.output().next.at == Catch::Approx(rested + 0.15));
    CHECK_FALSE(h.output().next.now);

    h.step(7);
    CHECK(due == 0);
    h.step(2);
    CHECK(due > 0);
    h.step();
    CHECK(drawn(h, tip));
    // The tooltip under the pointer's way doesn't take the hover.
    const int before = hovered;
    h.step();
    CHECK(hovered == before + 1);

    // Away, and gone.
    h.move_to({700.0f, 550.0f});
    CHECK_FALSE(drawn(h, tip));
}

TEST_CASE("A disabled widget takes nothing, but resting on it still shows its tooltip", "[ui-core][layers]") {
    UiHarness h;
    int due = 0;
    int clicked = 0;
    int hovered = 0;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        ui.push_disabled(true);
        const core::Interaction in = widget(h, "Upload");
        clicked += in.clicked ? 1 : 0;
        hovered += in.hovered ? 1 : 0;
        if (ui.tooltip_due()) ++due;
        ui.pop_disabled();
    });
    h.step();
    h.move_to(h.rect("Upload").centre());
    h.step(12);
    CHECK(due > 0);
    CHECK(hovered == 0);
    h.click("Upload");
    CHECK(clicked == 0);
}

TEST_CASE("A long press shows the tooltip where there is no hover", "[ui-core][layers]") {
    UiHarness h;
    int due = 0;
    h.set_ui([&] {
        (void)widget(h, "node");
        if (h.ui().tooltip_due()) ++due;
    });
    h.step();
    h.long_press("node", 0.7);
    CHECK(due > 0);
}

TEST_CASE("A drag carries a typed payload, offered while over a target and delivered where let go", "[ui-core][layers]") {
    UiHarness h;
    int dragging = 0;
    int offered = 0;
    int wrong = 0;
    int over_panel = 0;
    std::string delivered;
    const ftk::ui::Color preview{0.1f, 0.6f, 0.9f, 1.0f};
    bool preview_drawn = false;
    h.set_ui([&] {
        core::Context& ui = h.ui();
        (void)widget(h, "asset");
        if (ui.drag_source()) {
            ++dragging;
            ui.carry("ASSET_PATH", "rock.mesh", 10);
            ui.begin_drag_preview();
            plate(ui, preview);
            (void)widget(h, "preview", {80.0f, 20.0f});
            ui.end_drag_preview();
        }
        ui.layout().spacing(100.0f);
        (void)widget(h, "slot");
        if (auto drop = ui.drop_target("ASSET_PATH")) {
            ++offered;
            if (drop->delivered) delivered = std::string(drop->payload.text());
        }
        if (ui.drop_target("MATERIAL")) ++wrong;
        if (ui.drop_target(ui.panel_rect(), "ASSET_PATH")) ++over_panel;
    });
    h.step();
    h.drag(h.rect("asset").centre(), h.rect("slot").centre());
    CHECK(dragging >= 4);
    CHECK(offered >= 1);
    CHECK(delivered == "rock.mesh");
    CHECK(wrong == 0);
    CHECK(over_panel > offered);
    h.step();
    CHECK_FALSE(h.ui().dragging());

    // Let go anywhere else: nothing delivered.
    delivered.clear();
    h.drag(h.rect("asset").centre(), {600.0f, 500.0f});
    CHECK(delivered.empty());

    // The preview is drawn at the pointer while dragging.
    h.move_to(h.rect("asset").centre());
    h.send({.kind = PointerEvent::Kind::Down, .sample = {.position = h.rect("asset").centre()}});
    h.step();
    h.send({.kind = PointerEvent::Kind::Move, .sample = {.position = {300.0f, 300.0f}}});
    h.step(2);
    preview_drawn = drawn(h, preview);
    CHECK(preview_drawn);
    CHECK(h.ui().drag_type() == "ASSET_PATH");
    h.send({.kind = PointerEvent::Kind::Up, .sample = {.position = {300.0f, 300.0f}}});
    h.step(2);
    CHECK_FALSE(h.ui().dragging());
}
