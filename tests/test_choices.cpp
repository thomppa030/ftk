#include "kit_harness.hpp"

#include "ftk/ui/kit/choice.hpp"
#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/color_field.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/selectable.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/core/draw_list.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

using Catch::Approx;
using ftk::test::KitHarness;
using ftk::ui::core::Rect;
namespace ui = ftk::ui;

namespace {

enum class Shape { Box, Sphere, Capsule, Mesh };
constexpr ui::Choice<Shape> SHAPES[] = {
    {Shape::Box, "Box"}, {Shape::Sphere, "Sphere"}, {Shape::Capsule, "Capsule"}, {Shape::Mesh, "Mesh"},
};

enum class Body { Static, Dynamic, Kinematic };
constexpr ui::Choice<Body> BODIES[] = {
    {Body::Static, "Static"}, {Body::Dynamic, "Dynamic"}, {Body::Kinematic, "Kinematic"},
};

struct Tally {
    int changed{0};
    int committed{0};
    void add(const ui::Edit& edit) {
        if (edit.changed) ++changed;
        if (edit.committed) ++committed;
    }
};

// The popup a combo or picker just opened, once it has had the frames it
// takes to size itself to its contents.
std::optional<Rect> open_popup(KitHarness& h) {
    h.step(2);
    return h.popup();
}

// The spacing the kit's lists and popups are laid out with, read inside a
// frame.
struct Spacing {
    float line{0.0f};
    glm::vec2 item{0.0f};
    glm::vec2 inner{0.0f};
    glm::vec2 window{0.0f};
    float frame{0.0f};

    void read() {
        line = ui::line_height();
        item = ui::item_spacing();
        inner = ui::item_inner_spacing();
        window = ui::window_padding();
        frame = ui::frame_height();
    }
};

// The centre of the `index`th line of a popup list.
glm::vec2 popup_line(const Rect& popup, const Spacing& s, int index) {
    return {popup.centre().x,
            popup.min.y + s.window.y + static_cast<float>(index) * (s.line + s.item.y) + s.line * 0.5f};
}

void erase_and_type(KitHarness& h, const char* text) {
    for (int i = 0; i < 12; ++i) h.press(ui::Key::Backspace);
    h.type(text);
    h.press(ui::Key::Enter);
}

} // namespace

TEST_CASE("A drop-down sets the picked value as one commit", "[ui][kit]") {
    KitHarness h;
    Shape shape = Shape::Box;
    Tally tally;
    Spacing spacing;
    h.set_ui([&] {
        spacing.read();
        ui::next_width(200.0f);
        tally.add(ui::combo("##shape", shape, SHAPES));
        h.mark("combo");
    });
    h.step(2);
    h.click("combo");
    const auto popup = open_popup(h);
    REQUIRE(popup.has_value());
    h.click_at(popup_line(*popup, spacing, 2));
    CHECK(shape == Shape::Capsule);
    CHECK(tally.changed == 1);
    CHECK(tally.committed == 1);
}

TEST_CASE("Picking the value a drop-down already has is no edit", "[ui][kit]") {
    KitHarness h;
    Shape shape = Shape::Sphere;
    Tally tally;
    Spacing spacing;
    h.set_ui([&] {
        spacing.read();
        ui::next_width(200.0f);
        tally.add(ui::combo("##shape", shape, SHAPES));
        h.mark("combo");
    });
    h.step(2);
    h.click("combo");
    const auto popup = open_popup(h);
    REQUIRE(popup.has_value());
    h.click_at(popup_line(*popup, spacing, 1));
    CHECK(shape == Shape::Sphere);
    CHECK(tally.changed == 0);
}

TEST_CASE("Mixed, a drop-down sets even the value it holds", "[ui][kit]") {
    KitHarness h;
    Shape shape = Shape::Sphere;
    Tally tally;
    Spacing spacing;
    bool mixed = true;
    h.set_ui([&] {
        spacing.read();
        ui::next_width(200.0f);
        if (mixed) ui::next_field_mixed();
        const ui::Edit edit = ui::combo("##shape", shape, SHAPES);
        tally.add(edit);
        if (edit.changed) mixed = false;
        h.mark("combo");
    });
    h.step(2);
    h.click("combo");
    const auto popup = open_popup(h);
    REQUIRE(popup.has_value());
    // The value the field held, picked while several differ, sets them all.
    h.click_at(popup_line(*popup, spacing, 1));
    CHECK(shape == Shape::Sphere);
    CHECK(tally.changed == 1);
    CHECK(tally.committed == 1);
}

TEST_CASE("Mixed, no segment is lit and any click sets it", "[ui][kit]") {
    KitHarness h;
    Body body = Body::Kinematic;
    Tally tally;
    h.set_ui([&] {
        ui::next_width(300.0f);
        ui::next_field_mixed();
        tally.add(ui::segmented("##body", body, BODIES));
        h.mark("control");
    });
    h.step(2);
    for (const char* name : {"Static", "Dynamic", "Kinematic"}) {
        const auto* segment = h.find(name, ftk::ui::core::Role::Button);
        REQUIRE(segment != nullptr);
        CHECK_FALSE(segment->state.selected);
    }
    const Rect control = h.rect("control");
    h.click_at({control.min.x + control.width() * 5.0f / 6.0f, control.centre().y});
    CHECK(body == Body::Kinematic);
    CHECK(tally.committed == 1);
}

TEST_CASE("A segment click picks its value as one commit", "[ui][kit]") {
    KitHarness h;
    Body body = Body::Static;
    Tally tally;
    h.set_ui([&] {
        ui::next_width(300.0f);
        tally.add(ui::segmented("##body", body, BODIES));
        h.mark("control");
    });
    h.step(2);
    const Rect control = h.rect("control");
    const float y = control.centre().y;
    h.click_at({control.min.x + control.width() * 5.0f / 6.0f, y});
    CHECK(body == Body::Kinematic);
    CHECK(tally.committed == 1);
    h.click_at({control.min.x + control.width() * 5.0f / 6.0f, y});
    CHECK(tally.changed == 1);
    h.click_at({control.min.x + control.width() * 0.5f, y});
    CHECK(body == Body::Dynamic);
    CHECK(tally.committed == 2);
}

TEST_CASE("Each segment is recorded where it is drawn, and found there", "[ui][kit]") {
    KitHarness h;
    Body body = Body::Static;
    h.set_ui([&] {
        ui::next_width(300.0f);
        (void)ui::segmented("##body", body, BODIES);
    });
    h.step(2);
    const auto* stat = h.find("Static", ftk::ui::core::Role::Button);
    const auto* dynamic = h.find("Dynamic", ftk::ui::core::Role::Button);
    const auto* kinematic = h.find("Kinematic", ftk::ui::core::Role::Button);
    REQUIRE((stat != nullptr && dynamic != nullptr && kinematic != nullptr));
    CHECK(stat->rect.max.x <= dynamic->rect.min.x);
    CHECK(dynamic->rect.max.x <= kinematic->rect.min.x);
    h.click_at(kinematic->rect.centre());
    CHECK(body == Body::Kinematic);
    h.click_at(h.find("Static", ftk::ui::core::Role::Button)->rect.centre());
    CHECK(body == Body::Static);
}

TEST_CASE("A choice is segmented only for three short options that fit", "[ui][kit]") {
    KitHarness h;
    const std::array<const char* const, 3> three{"Static", "Dynamic", "Kinematic"};
    const std::array<const char* const, 4> four{"Box", "Sphere", "Capsule", "Mesh"};
    bool wide = false;
    bool narrow = true;
    bool too_many = true;
    h.set_ui([&] {
        ui::next_width(300.0f);
        wide = ui::detail::fits_segmented(three);
        ui::next_width(90.0f);
        narrow = ui::detail::fits_segmented(three);
        ui::next_width(600.0f);
        too_many = ui::detail::fits_segmented(four);
    });
    h.step(2);
    CHECK(wide);
    CHECK_FALSE(narrow);
    CHECK_FALSE(too_many);
}

TEST_CASE("A choice over plain names picks by index, as a script's enum does", "[ui][kit]") {
    KitHarness h;
    const std::array<const char* const, 2> modes{"Walk", "Run"};
    int index = 0;
    Tally tally;
    h.set_ui([&] {
        ui::next_width(300.0f);
        tally.add(ui::choice("##mode", index, modes));
        h.mark("control");
    });
    h.step(2);
    const Rect control = h.rect("control");
    // Two short names fit, so both show side by side: the right half is "Run".
    h.click_at({control.min.x + control.width() * 0.75f, control.centre().y});
    CHECK(index == 1);
    CHECK(tally.committed == 1);
}

TEST_CASE("A typed hex code sets a linear colour through sRGB", "[ui][kit]") {
    KitHarness h;
    glm::vec3 light{1.0f, 1.0f, 1.0f};
    Tally tally;
    h.set_ui([&] {
        ui::next_width(240.0f);
        tally.add(ui::color("##light", light, ui::ColorSpace::Linear));
        h.mark("hex");
    });
    h.step(2);
    h.click("hex");
    erase_and_type(h, "#FF8000");
    CHECK(light.r == Approx(1.0f));
    CHECK(light.g == Approx(0.2158f).margin(0.001f));  // sRGB 0x80 in linear light
    CHECK(light.b == Approx(0.0f));
    CHECK(tally.committed == 1);
}

TEST_CASE("A hex code that doesn't parse leaves the colour alone", "[ui][kit]") {
    KitHarness h;
    glm::vec4 tint{0.25f, 0.5f, 0.75f, 1.0f};
    Tally tally;
    h.set_ui([&] {
        ui::next_width(240.0f);
        tally.add(ui::color("##tint", tint, ui::ColorSpace::Srgb));
        h.mark("hex");
    });
    h.step(2);
    h.click("hex");
    erase_and_type(h, "#12G456");
    CHECK(tint == glm::vec4{0.25f, 0.5f, 0.75f, 1.0f});
    CHECK(tally.committed == 0);
}

TEST_CASE("Dragging in the colour picker commits once, on release", "[ui][kit]") {
    KitHarness h;
    glm::vec3 tint{0.5f, 0.5f, 0.5f};
    Tally tally;
    Spacing spacing;
    h.set_ui([&] {
        spacing.read();
        ui::next_width(240.0f);
        tally.add(ui::color("##tint", tint, ui::ColorSpace::Srgb));
        h.mark("hex");
    });
    h.step(2);
    // The swatch sits left of the hex code and opens the picker.
    const glm::vec2 hex = h.rect("hex").min;
    const float side = spacing.frame;
    h.click_at({hex.x - spacing.inner.x - side * 0.5f, hex.y + side * 0.5f});
    const auto popup = open_popup(h);
    REQUIRE(popup.has_value());
    // The saturation/value square is the picker's top-left.
    const glm::vec2 corner = popup->min + spacing.window;
    h.drag({corner.x + 20.0f, corner.y + 20.0f}, {corner.x + 60.0f, corner.y + 50.0f});
    CHECK(tint != glm::vec3{0.5f, 0.5f, 0.5f});
    CHECK(tally.changed > 1);
    CHECK(tally.committed == 1);
}

TEST_CASE("A drop-down's list is the caller's, and an entry chosen closes it", "[ui][kit]") {
    KitHarness h;
    int picked = -1;
    bool any_open = false;
    float width = 0.0f;
    h.set_ui([&] {
        {
            ui::next_width(160.0f);
            ui::Dropdown list("##event", "Pick an event...");
            if (!list) {
                h.mark("dropdown");
                width = ui::item_max().x - ui::item_min().x;
            }
            if (list) {
                for (int i = 0; i < 2; ++i) {
                    ui::IdScope entry(i);
                    if (ui::selectable(i == 0 ? "Storm" : "Fog", false)) picked = i;
                    h.mark("entry" + std::to_string(i));
                }
            }
        }
        any_open = ui::any_popup_open();
    });
    h.step(2);
    CHECK(width == 160.0f);
    CHECK_FALSE(any_open);
    h.click("dropdown");
    h.step();
    CHECK(any_open);
    h.click("entry1");
    h.step();
    CHECK(picked == 1);
    CHECK_FALSE(any_open);
}

TEST_CASE("A swatch is clicked, and the picker writes the colour it is dragged to", "[ui][kit]") {
    KitHarness h;
    int picked = 0;
    glm::vec3 colour{0.2f, 0.4f, 0.6f};
    ui::Edit last{};
    bool committed = false;
    h.set_ui([&] {
        if (ui::swatch("##amber", {0.83f, 0.63f, 0.31f, 1.0f}, {20.0f, 20.0f})) ++picked;
        h.mark("swatch");
        ui::bullet_text("textures/rock.png");
        ui::next_width(200.0f);
        last = ui::color_picker("##custom", colour);
        committed = committed || last.committed;
        h.mark("picker");
    });
    h.step(2);
    CHECK(h.rect("swatch").width() == 20.0f);
    h.click("swatch");
    CHECK(picked == 1);
    // A press near the square's top-left corner: white, the brightest and
    // least saturated.
    const glm::vec2 corner = h.rect("picker").min + glm::vec2{2.0f, 2.0f};
    h.click_at(corner);
    h.step();
    CHECK(colour.r > 0.9f);
    CHECK(colour.g > 0.9f);
    CHECK(colour.b > 0.9f);
    CHECK(committed);
}

TEST_CASE("The anchor picker shows the point picked and picks the dot clicked", "[ui][kit]") {
    KitHarness h;
    ui::AnchorPick pick{.column = 0, .row = 2};
    int changed = 0;
    h.set_ui([&] {
        if (ui::anchor_picker("##anchor", pick).changed) ++changed;
        h.mark("picker");
    });
    h.step(2);
    const Rect box = h.rect("picker");
    CHECK(box.width() == Approx(78.0f));
    CHECK(box.height() == Approx(52.0f));
    // The mark, 12 x 8 in the toggle colour, at the bottom left dot.
    const auto marks = [&] {
        std::vector<Rect> found;
        const uint32_t toggle = ui::core::pack_colour(ftk::theme::toggle_on());
        for (const auto& root : h.output().roots) {
            for (const auto& p : root.draw->primitives) {
                if (p.kind == ui::core::PrimitiveKind::Rect && p.colour == toggle) found.push_back({p.min, p.max});
            }
        }
        return found;
    };
    REQUIRE(marks().size() == 1);
    CHECK(marks()[0].width() == Approx(12.0f));
    CHECK(marks()[0].min.x == Approx(box.min.x + 13.0f - 6.0f));
    CHECK(marks()[0].min.y == Approx(box.min.y + 43.0f - 4.0f));

    // The middle dot clicked: Centre.
    const glm::vec2 centre = box.min + glm::vec2{39.0f, 26.0f};
    h.drag(centre, centre);
    h.step(1);
    CHECK(pick == ui::AnchorPick{.column = 1, .row = 1});
    CHECK(changed == 1);

    // Across its row, a bar; a dot clicked picks a point again.
    pick = {.column = 0, .row = 0, .across = true};
    h.step(1);
    REQUIRE(marks().size() == 1);
    CHECK(marks()[0].width() == Approx(66.0f));
    const glm::vec2 right = box.min + glm::vec2{65.0f, 9.0f};
    h.drag(right, right);
    h.step(1);
    CHECK(pick == ui::AnchorPick{.column = 2, .row = 0});
}
