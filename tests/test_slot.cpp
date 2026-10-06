#include "kit_harness.hpp"

#include "ftk/ui/kit/asset_kind.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/drag_drop.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/slot.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using ftk::test::KitHarness;
using namespace ftk;

namespace {

constexpr const char* PAYLOAD = "TEST_ITEM";

// A slot taking named items, beside a button that drags `dragged` the way a
// list would. It refuses anything named "crate".
struct SlotUnderTest {
    KitHarness h;
    std::string held;
    std::string kept;
    std::string dragged;
    int clicks{0};
    int clears{0};
    glm::vec2 field{};
    float frame{0.0f};

    SlotUnderTest() {
        h.set_ui([this] {
            ui::button("Item in a list");
            h.mark("source");
            if (auto drag = ui::DragSource()) drag.carry(PAYLOAD, dragged);
            field = ui::cursor();
            frame = ui::frame_height();
            ui::next_width(240.0f);
            const ui::IdScope id("##item");
            ui::Slot slot(PAYLOAD, [](const ui::Payload& payload) -> std::string {
                const std::string name = static_cast<const char*>(payload.data);
                return name == "crate" ? "Crates do not fit." : std::string();
            });
            h.mark("field");
            if (slot.clicked()) ++clicks;
            if (const ui::Payload* dropped = slot.dropped()) held = static_cast<const char*>(dropped->data);
            std::string text = held.empty() ? "None" : held;
            if (slot.finish({.text = text}, held.empty(), {}, kept)) {
                held.clear();
                ++clears;
            }
            h.mark("clear");
        });
        h.step(2);
    }

    [[nodiscard]] glm::vec2 into_field() const { return {field.x + 20.0f, field.y + frame * 0.5f}; }
    [[nodiscard]] glm::vec2 source() const { return h.rect("source").centre(); }
};

} // namespace

TEST_CASE("A slot hands over what is dropped on it once it fits", "[ui][slot]") {
    SlotUnderTest t;
    t.dragged = "crate";
    t.h.drag(t.source(), t.into_field());
    CHECK(t.held.empty());

    // The payload is read after the drop target has closed: the slot keeps
    // a copy.
    t.dragged = "goblin";
    t.h.drag(t.source(), t.into_field());
    CHECK(t.held == "goblin");
}

TEST_CASE("Clicking a slot's field asks for its picker", "[ui][slot]") {
    SlotUnderTest t;
    t.h.click("field");
    CHECK(t.clicks == 1);
}

TEST_CASE("A slot's clear button empties it, and does nothing while it is empty", "[ui][slot]") {
    SlotUnderTest t;
    t.h.click("clear");
    CHECK(t.clears == 0);

    t.held = "goblin";
    t.h.step();
    t.h.click("clear");
    CHECK(t.clears == 1);
    CHECK(t.held.empty());
}

TEST_CASE("A slot that must hold something cannot be cleared", "[ui][slot]") {
    SlotUnderTest t;
    t.held = "goblin";
    t.kept = "It always holds a goblin";
    t.h.step();
    t.h.click("clear");
    CHECK(t.clears == 0);
    CHECK(t.held == "goblin");
}

TEST_CASE("A built-in reads as the kind its slot takes, by name, and is never missing", "[ui][slot]") {
    // A name with no extension of its own: held, it would read as a plain file.
    std::string text;
    const ui::SlotFace face = ui::asset_face("White", ui::AssetPresence::built_in, ".png", text);
    CHECK(face.text == "White");
    CHECK(face.glyph == ui::asset_kind(".png").icon);
    CHECK(face.glyph != ui::icon::warning);
    const ui::Color colour = theme::text();
    CHECK(face.text_colour.x == colour.x);
    CHECK(face.text_colour.y == colour.y);
    CHECK(face.text_colour.z == colour.z);
}
