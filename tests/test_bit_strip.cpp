#include "kit_harness.hpp"

#include "ftk/ui/kit/bit_strip.hpp"
#include "ftk/ui/kit/layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// A strip of four: one set, one inherited, one that can be set and one that
// cannot.
struct Fixture {
    KitHarness h;
    std::array<ui::Bit, 4> bits{ui::Bit{.state = ui::BitState::on}, ui::Bit{.state = ui::BitState::inherited},
                                ui::Bit{.state = ui::BitState::off}, ui::Bit{.state = ui::BitState::unavailable}};
    int clicked{-1};
    glm::vec2 origin{};
    float frame{0.0f};

    Fixture() {
        h.set_ui([this] {
            origin = ui::cursor();
            frame = ui::frame_height();
            if (const auto bit = ui::bit_strip("##bits", bits)) clicked = static_cast<int>(*bit);
        });
        h.step(3);
    }

    // The centre of square `i`: 30 px wide, 4 px apart, a frame tall.
    [[nodiscard]] glm::vec2 square(int i) const {
        return {origin.x + 15.0f + 34.0f * static_cast<float>(i), origin.y + frame * 0.5f};
    }
};

} // namespace

TEST_CASE("A bit that can be set or cleared reports its click", "[ui][kit]") {
    Fixture f;
    f.h.click_at(f.square(0));
    CHECK(f.clicked == 0);
    f.h.click_at(f.square(2));
    CHECK(f.clicked == 2);
}

TEST_CASE("A strip of eight in lines of four numbers on into the second line", "[ui][kit]") {
    KitHarness h;
    std::array<ui::Bit, 8> bits{};
    bits.fill(ui::Bit{.state = ui::BitState::off});
    int clicked = -1;
    glm::vec2 origin{};
    float frame = 0.0f;
    h.set_ui([&] {
        origin = ui::cursor();
        frame = ui::frame_height();
        if (const auto bit = ui::bit_strip("##bits", bits, 4)) clicked = static_cast<int>(*bit);
    });
    h.step(3);
    // Square 5 is under square 1, a gap below it; square 8 under square 4.
    const auto square = [&](int column, int line) {
        return glm::vec2{origin.x + 15.0f + 34.0f * static_cast<float>(column),
                         origin.y + frame * 0.5f + (frame + 4.0f) * static_cast<float>(line)};
    };
    h.click_at(square(0, 1));
    CHECK(clicked == 4);
    h.click_at(square(3, 1));
    CHECK(clicked == 7);
    h.click_at(square(2, 0));
    CHECK(clicked == 2);
    // Nothing is to the right of square 4 on the first line.
    clicked = -1;
    h.click_at(square(4, 0));
    CHECK(clicked == -1);
}

TEST_CASE("An inherited or unnamed bit does nothing when clicked", "[ui][kit]") {
    Fixture f;
    f.h.click_at(f.square(1));
    CHECK(f.clicked == -1);
    f.h.click_at(f.square(3));
    CHECK(f.clicked == -1);
}
