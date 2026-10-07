#include "kit_harness.hpp"

#include "ftk/ui/kit/field.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/list_editor.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <string>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

// A list_editor of numbers in the harness. Each card marks its content; the
// list's right edge is kept to aim at the trash icons.
struct Fixture {
    KitHarness h;
    std::vector<int> items{10, 20, 30};
    int commits{0};
    float right{0.0f};
    float frame{0.0f};
    float padding{0.0f};

    Fixture() {
        h.set_ui([this] {
            right = ui::cursor().x + ui::space_left().x;
            frame = ui::frame_height();
            padding = ui::frame_padding().y;
            const ui::Edit edit = ui::list_editor(
                "##numbers", items, "Add", "Nothing",
                [this](int&, std::size_t i) {
                    ui::text("item");
                    h.mark("content" + std::to_string(i));
                    return ui::Edit{};
                });
            h.mark("add");
            if (edit.committed) ++commits;
        });
        h.step(3);
    }

    // The centre of card `i`'s trash icon: the frame-height square at the
    // card's right edge, inset by the card's padding.
    [[nodiscard]] glm::vec2 trash(int i) const {
        const float top = line_top(i);
        return {right - 4.0f - frame * 0.5f, top + frame * 0.5f};
    }
    // A point on card `i`'s grip, left of its number and content.
    [[nodiscard]] glm::vec2 grip(int i) const {
        const glm::vec2 content = h.rect("content" + std::to_string(i)).min;
        return {content.x - 36.0f, content.y + 8.0f};
    }
    // The top of card `i`'s first line: its text sits a frame's padding
    // below it.
    [[nodiscard]] float line_top(int i) const { return h.rect("content" + std::to_string(i)).min.y - padding; }
    // Card `i`'s top and bottom.
    [[nodiscard]] float top(int i) const { return line_top(i) - 4.0f; }
    [[nodiscard]] float bottom(int i) const { return top(i) + 4.0f + frame + 4.0f; }
};

} // namespace

TEST_CASE("A list's trash icon removes that item, as a finished edit", "[ui][kit]") {
    Fixture f;
    f.h.click_at(f.trash(1));
    CHECK(f.items == std::vector{10, 30});
    CHECK(f.commits == 1);
}

TEST_CASE("A list's Add button appends an item, as a finished edit", "[ui][kit]") {
    Fixture f;
    f.h.click("add");
    CHECK(f.items == std::vector{10, 20, 30, 0});
    CHECK(f.commits == 1);
}

TEST_CASE("Dragging a card onto the lower half of a later one moves it after that one", "[ui][kit]") {
    Fixture f;
    const glm::vec2 grip = f.grip(0);
    f.h.drag(grip, {grip.x + 20.0f, f.bottom(2) - 3.0f});
    CHECK(f.items == std::vector{20, 30, 10});
    CHECK(f.commits == 1);
}

TEST_CASE("Dragging a card onto the upper half of an earlier one moves it before that one",
          "[ui][kit]") {
    Fixture f;
    const glm::vec2 grip = f.grip(2);
    f.h.drag(grip, {grip.x + 20.0f, f.top(1) + 3.0f});
    CHECK(f.items == std::vector{10, 30, 20});
    CHECK(f.commits == 1);
}

TEST_CASE("Dropping a card where it already is changes nothing", "[ui][kit]") {
    Fixture f;
    const glm::vec2 grip = f.grip(1);
    f.h.drag(grip, {grip.x + 20.0f, f.bottom(1) - 3.0f});
    CHECK(f.items == std::vector{10, 20, 30});
    CHECK(f.commits == 0);
}

namespace {

// A selectable list of numbers: each card counts how often its summary and
// its fields are drawn, and marks its summary line.
struct SelectableFixture {
    KitHarness h;
    std::vector<int> items{10, 20, 30};
    int selected{-1};
    int fields_drawn{0};
    int summaries_drawn{0};
    float frame{0.0f};
    float window_padding{0.0f};

    SelectableFixture() {
        h.set_ui([this] {
            frame = ui::frame_height();
            window_padding = ui::window_padding().x;
            (void)ui::selectable_list_editor(
                "##select", items, selected, "Add", "Nothing", {},
                [this](const int&, std::size_t i) {
                    ++summaries_drawn;
                    ui::text("summary");
                    h.mark("summary" + std::to_string(i));
                },
                [this](int&, std::size_t i) {
                    ++fields_drawn;
                    ui::text("fields");
                    h.mark("summary" + std::to_string(i));
                    return ui::Edit{};
                },
                [] { return 0; });
        });
        h.step(3);
    }
};

} // namespace

TEST_CASE("Clicking a card selects it, and clicking it again lets it go", "[ui][kit]") {
    SelectableFixture f;
    CHECK(f.selected == -1);
    f.h.click("summary1");
    CHECK(f.selected == 1);
    f.h.click("summary1");
    CHECK(f.selected == -1);
}

TEST_CASE("Only the selected card draws its fields", "[ui][kit]") {
    SelectableFixture f;
    f.h.click("summary2");
    f.fields_drawn = 0;
    f.summaries_drawn = 0;
    f.h.step();
    CHECK(f.fields_drawn == 1);
    CHECK(f.summaries_drawn == 2);
}

TEST_CASE("Removing a card before the selected one keeps the selection on its item", "[ui][kit]") {
    SelectableFixture f;
    f.h.click("summary2");
    // The trash icon of card 0: the frame-height square at the card's right.
    const glm::vec2 min = f.h.rect("summary0").min;
    const float side = f.frame;
    const float right = f.h.size().x - f.window_padding;
    f.h.click_at({right - 4.0f - side * 0.5f, min.y + side * 0.5f});
    CHECK(f.items == std::vector{20, 30});
    CHECK(f.selected == 1);
}

TEST_CASE("Cards out of sight are not drawn", "[ui][kit]") {
    KitHarness h;
    std::vector<int> items(500, 0);
    int selected = -1;
    int drawn = 0;
    h.set_ui([&] {
        (void)ui::selectable_list_editor(
            "##long", items, selected, "Add", "Nothing", {.max_cards = 8},
            [&](const int&, std::size_t) { ++drawn; },
            [&](int&, std::size_t) { ++drawn; return ui::Edit{}; }, [] { return 0; });
    });
    h.step(3);
    drawn = 0;
    h.step();
    CHECK(drawn > 0);
    CHECK(drawn <= 10);
}

namespace {

// A list of `cards` cards that open, in a box `box` cards tall (0 for none);
// card `open_card` shows two lines of fields under its first. Its loop goes
// through in_sight(), or with `walk` over every index, as a list whose
// closed cards differ in height does: where the walked list puts its cards
// is where the counted one must.
struct OpenCards {
    KitHarness h;
    std::size_t count;
    int open;
    bool walked;
    int max_cards;
    // Each card drawn on the last frame, by its first line's top below the
    // list's top.
    std::map<std::size_t, float> drawn;
    // How many cards the loop came to on the last frame.
    std::size_t visited{0};
    float top{0.0f};
    // The Add button's top below the list's top.
    float add{0.0f};

    OpenCards(std::size_t cards, int open_card, bool walk, int box = 8)
        : count{cards}, open{open_card}, walked{walk}, max_cards{box} {
        h.set_ui([this] {
            drawn.clear();
            visited = 0;
            top = ui::cursor().y;
            ui::ListEditor list("##cards", count, {.reorder = false, .max_cards = max_cards, .opens = true});
            const auto card = [&](std::size_t i) {
                ++visited;
                const bool is_open = open == static_cast<int>(i);
                if (!list.begin_item(i, is_open)) return;
                ui::text("Card");
                drawn[i] = h.ui().layout().last_item().min.y - top;
                if (is_open) {
                    ui::text("Field");
                    ui::text("Field");
                }
                (void)list.end_item();
            };
            if (walked) {
                for (std::size_t i = 0; i < count; ++i) card(i);
            } else {
                for (const std::size_t i : list.in_sight(open)) card(i);
            }
            (void)list.add_button("Add");
            add = h.ui().layout().last_item().min.y - top;
        });
        h.step(3);
    }

    // Turns the wheel over the list's first card and lets the scroll settle.
    void scroll(float notches) {
        h.wheel({h.size().x * 0.5f, top + 20.0f}, notches);
        h.step(60);
    }
};

// Where a list of 2000 cards, the fourth open, draws its cards: at the top,
// scrolled well past the open card, and scrolled to the end.
std::vector<std::map<std::size_t, float>> boxed_places(bool walked) {
    OpenCards list(2000, 3, walked);
    std::vector<std::map<std::size_t, float>> places{list.drawn};
    list.scroll(-40.0f);
    places.push_back(list.drawn);
    list.scroll(-100000.0f);
    places.push_back(list.drawn);
    return places;
}

void check_same_places(const std::map<std::size_t, float>& walked, const std::map<std::size_t, float>& counted) {
    REQUIRE(!counted.empty());
    REQUIRE(counted.size() == walked.size());
    for (const auto& [card, y] : walked) {
        INFO("card " << card);
        REQUIRE(counted.contains(card));
        CHECK(std::abs(counted.at(card) - y) < 0.5f);
    }
}

} // namespace

TEST_CASE("A list whose cards open draws the cards in sight where visiting every card puts them", "[ui][kit]") {
    const auto walked = boxed_places(true);
    const auto counted = boxed_places(false);
    // The open card is in sight at the top, and far above it once scrolled.
    CHECK(counted[0].contains(3));
    CHECK(counted[1].begin()->first > 10);
    // Scrolled to the end, the last card shows: the box holds all of them.
    CHECK(counted[2].contains(1999));
    for (std::size_t i = 0; i < walked.size(); ++i) {
        INFO("place " << i);
        check_same_places(walked[i], counted[i]);
    }
}

TEST_CASE("A list whose cards open comes to the cards in sight alone, however many it holds", "[ui][kit]") {
    OpenCards list(100000, -1, false);
    CHECK(list.visited <= 10);
    list.scroll(-1000.0f);
    REQUIRE(!list.drawn.empty());
    CHECK(list.drawn.begin()->first > 100);
    CHECK(list.visited <= 10);
}

TEST_CASE("A list whose cards open, without a box, ends where visiting every card ends it", "[ui][kit]") {
    OpenCards walked(300, 2, true, 0);
    OpenCards counted(300, 2, false, 0);
    CHECK(counted.visited < 30);
    CHECK(std::abs(counted.add - walked.add) < 0.5f);
    check_same_places(walked.drawn, counted.drawn);
}

TEST_CASE("An index follows its item when another moves past it", "[ui][kit]") {
    // Item 0 moved in front of 3: items 1 and 2 shift down.
    CHECK(ui::index_after_move(0, 0, 3) == 2);
    CHECK(ui::index_after_move(1, 0, 3) == 0);
    CHECK(ui::index_after_move(3, 0, 3) == 3);
    // Item 3 moved in front of 1: items 1 and 2 shift up.
    CHECK(ui::index_after_move(3, 3, 1) == 1);
    CHECK(ui::index_after_move(1, 3, 1) == 2);
    CHECK(ui::index_after_move(0, 3, 1) == 0);
}

TEST_CASE("A card whose first line is text centres it on the number's line", "[ui][kit]") {
    KitHarness h;
    std::vector<int> items{10};
    float list_top = 0.0f;
    float frame = 0.0f;
    h.set_ui([&] {
        list_top = ui::cursor().y;
        frame = ui::frame_height();
        (void)ui::list_editor("##numbers", items, "Add", "Nothing", [&](int&, std::size_t) {
            ui::text("Sphere on Head");
            h.mark("text");
            return ui::Edit{};
        });
    });
    h.step(3);
    // The number, grip and trash icon sit on a field line under the card's
    // 4 px padding; the text's middle is that line's middle.
    const float text_middle = h.rect("text").centre().y;
    const float line_middle = list_top + 4.0f + frame * 0.5f;
    CHECK(std::abs(text_middle - line_middle) <= 1.0f);
}

TEST_CASE("A closed card that opens reads dim, and in full text under the mouse", "[ui][kit]") {
    KitHarness h;
    std::vector<int> items{10, 20};
    int selected = 0;
    ui::Color summary_colour{};
    h.set_ui([&] {
        (void)ui::selectable_list_editor(
            "##points", items, selected, "Add", "Nothing", {},
            [&](int&, std::size_t) {
                ui::text("Point");
                summary_colour = ui::text_colour();
                h.mark("summary");
            },
            [&](int&, std::size_t) { return ui::Edit{}; }, [] { return 0; });
    });
    h.step(3);
    CHECK(summary_colour == ftk::theme::text_secondary());

    const glm::vec2 min = h.rect("summary").min;
    h.move_to({min.x + 2.0f, min.y + 2.0f});
    h.step();
    CHECK(summary_colour == ftk::theme::text());
}

namespace {

// Selectable cards headed by card_heading(), each with a tag and a summary.
struct HeadedFixture {
    KitHarness h;
    std::vector<int> items{10, 20};
    int selected{-1};
    float right{0.0f};
    float frame{0.0f};

    HeadedFixture() {
        h.set_ui([this] {
            right = ui::cursor().x + ui::space_left().x;
            frame = ui::frame_height();
            (void)ui::selectable_list_editor(
                "##headed", items, selected, "Add", "Nothing", {},
                [this](const int&, std::size_t i) {
                    ui::card_heading(nullptr, ftk::theme::Hue::blue, "name", "Tag");
                    h.mark("tag" + std::to_string(i));
                    ui::card_summary("summary");
                    h.mark("summary" + std::to_string(i));
                },
                [this](int&, std::size_t i) {
                    ui::card_heading(nullptr, ftk::theme::Hue::blue, "name", "Tag");
                    h.mark("tag" + std::to_string(i));
                    return ui::Edit{};
                },
                [] { return 0; });
        });
        h.step(3);
    }
};

} // namespace

TEST_CASE("A card heading's tag ends where the card's fields end", "[ui][kit]") {
    HeadedFixture f;
    // The fields end a gap short of the trash icon, a frame square inset by
    // the card's padding.
    const float fields_end = f.right - 4.0f - f.frame - ftk::theme::GAP_S;
    CHECK(std::abs(f.h.rect("tag0").max.x - fields_end) < 1.0f);
}

TEST_CASE("Clicking a headed card's summary selects the card", "[ui][kit]") {
    HeadedFixture f;
    f.h.click("summary1");
    CHECK(f.selected == 1);
}
