#include "kit_harness.hpp"

#include "ftk/ui/kit/asset_header.hpp"
#include "ftk/ui/kit/other_window.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/tabs.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

// A window of the program's own beside the one being drawn, which the
// program fills: an editor's tab torn out into one.

using Catch::Approx;
using ftk::test::KitHarness;
namespace ui = ftk::ui;

namespace {

constexpr const char* OTHER = "##other";

// What a frame drew into the window `root`, if it drew it.
const ui::core::RootOutput* drawn(const KitHarness& h, ui::core::RootId root) {
    const auto& roots = h.output().roots;
    const auto found = std::ranges::find(roots, root, &ui::core::RootOutput::root);
    return found == roots.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("A window of the program's own is asked for, drawn into, closed by its user and by the program",
          "[ui][kit][other_window]") {
    KitHarness h;
    bool wanted = true;
    bool take_back = false;
    bool drawn_in = false;
    bool told_closed = false;
    h.set_ui([&] {
        if (take_back) {
            ui::close_other_window(OTHER);
            take_back = false;
            wanted = false;
        }
        if (!wanted) return;
        const ui::OtherWindow other(OTHER, {.title = "bark.fjmat", .size = {400.0f, 300.0f}, .at = {20.0f, 30.0f}});
        if (other) {
            if (auto panel = ui::Panel("", "Inside")) ui::text("inside");
            drawn_in = true;
        }
        if (other.closed()) {
            told_closed = true;
            wanted = false;
        }
    });

    // Asked for the first frame, a window of its own beside the harness's.
    h.step();
    REQUIRE(h.windows().size() == 1);
    const ui::core::RootRequest asked = h.windows()[0];
    CHECK(asked.root == ui::other_window_root(OTHER));
    CHECK(asked.kind == ui::core::RootKind::Window);
    CHECK(asked.title == "bark.fjmat");
    CHECK(asked.size == glm::vec2{400.0f, 300.0f});
    CHECK(asked.at == glm::vec2{20.0f, 30.0f});
    CHECK(asked.near == KitHarness::WINDOW);
    CHECK_FALSE(drawn_in);

    // Drawn into once the program has opened it: what it holds, from the
    // frame after.
    h.step(2);
    CHECK(drawn_in);
    h.step();
    const ui::core::RootOutput* inside = drawn(h, asked.root);
    REQUIRE(inside != nullptr);
    CHECK_FALSE(inside->draw->primitives.empty());

    // Its user closes it: the program is told once, and it is gone.
    h.close_window(asked.root);
    h.step();
    CHECK(told_closed);
    h.step();
    CHECK(h.windows().empty());

    // Drawn again, it opens anew; taken back by the program, it closes.
    wanted = true;
    h.step();
    CHECK(h.windows().size() == 1);
    take_back = true;
    h.step(2);
    CHECK(h.windows().empty());
}

TEST_CASE("A window of the program's own takes its title as it changes, and is raised when asked",
          "[ui][kit][other_window]") {
    KitHarness h;
    std::string title = "bark.fjmat";
    bool raise = false;
    h.set_ui([&] {
        ui::OtherWindow other(OTHER, {.title = title, .size = {400.0f, 300.0f}});
        if (raise) other.raise();
    });
    h.step(3);
    REQUIRE(h.windows().size() == 1);
    CHECK(h.output().raise.empty());

    title = "bark.fjmat ●";
    h.step();
    REQUIRE(h.windows().size() == 1);
    CHECK(h.windows()[0].title == "bark.fjmat ●");
    CHECK(h.windows()[0].size == glm::vec2{400.0f, 300.0f});

    raise = true;
    h.step();
    CHECK(std::ranges::contains(h.output().raise, ui::other_window_root(OTHER)));
}

TEST_CASE("Each window has bars and an asset header of its own", "[ui][kit][other_window]") {
    KitHarness h;
    std::string main_header;
    std::string other_header;
    float other_work_top = -1.0f;
    const ui::AssetHeaderSpec spec{.name = "bark.fjmat"};
    h.set_ui([&] {
        { auto bar = ui::ContextTabBar("##bar"); }
        {
            auto header = ui::AssetHeaderBar(spec);
            main_header = ui::asset_header_window();
        }
        const ui::OtherWindow other(OTHER, {.title = "bark.fjmat", .size = {400.0f, 300.0f}});
        if (!other) return;
        { auto bar = ui::ContextTabBar("##other_bar"); }
        {
            auto header = ui::AssetHeaderBar(spec);
            other_header = ui::asset_header_window();
        }
        other_work_top = ui::work_area().pos.y;
    });
    h.step(4);

    // The other window's bars start at its own top, not under the main
    // window's.
    const auto bar = ui::window_named("##bar");
    const auto other_bar = ui::window_named("##other_bar");
    REQUIRE(bar.has_value());
    REQUIRE(other_bar.has_value());
    CHECK(other_bar->pos.y == Approx(0.0f));
    CHECK(other_bar->size.y == Approx(bar->size.y));

    // Its header is its own window, under its own tab bar.
    CHECK(main_header != other_header);
    const auto header = ui::window_named(main_header);
    const auto other_header_shown = ui::window_named(other_header);
    REQUIRE(header.has_value());
    REQUIRE(other_header_shown.has_value());
    CHECK(other_header_shown->pos.y == Approx(header->pos.y));
    CHECK(other_work_top == Approx(header->pos.y + header->size.y));
}
