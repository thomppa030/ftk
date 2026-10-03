#include "ftk/test/ui_harness.hpp"
#include "ftk/ui/core/context.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/vec2.hpp>

#include <string>

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
    REQUIRE(h.output().next.at.has_value());
    CHECK(*h.output().next.at == Catch::Approx(rested + 0.15));
    CHECK_FALSE(h.output().next.now);

    h.step(8);
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
