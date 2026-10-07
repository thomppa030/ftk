#include "kit_harness.hpp"

#include "ftk/app/console.hpp"
#include "ftk/base/problems.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>
#include <spdlog/logger.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace core = ftk::ui::core;

namespace {

const std::string SHADER = "/project/shaders/sea.fjsl";

struct Console {
    KitHarness h{{900.0f, 500.0f}};
    ftk::ProblemList list;
    std::shared_ptr<ftk::ConsoleSink> console = std::make_shared<ftk::ConsoleSink>(list);
    std::vector<ftk::Problem> opened;
    std::vector<ftk::Problem> revealed;

    Console() {
        console->set_problem_actions({.open = [this](const ftk::Problem& p) { opened.push_back(p); },
                                      .reveal = [this](const ftk::Problem& p) { revealed.push_back(p); },
                                      .reveal_label = "Show in content browser"});
        list.report("shader:sea", {{.severity = ftk::ProblemSeverity::Error,
                                    .message = "'foam_bias' : undeclared identifier",
                                    .meanwhile = "It draws with the last version that compiled",
                                    .file = SHADER,
                                    .line = 41}});
        list.report("sheet:hud", {{.severity = ftk::ProblemSeverity::Warning,
                                   .message = "Unknown property 'text-shadow'",
                                   .file = "/project/ui/hud.fjss",
                                   .line = 12,
                                   .column = 5}});
        list.report("font:title", {{.severity = ftk::ProblemSeverity::Error,
                                    .message = "Font didn't bake: file not found",
                                    .file = "/project/fonts/Title.fjfont"}});
        h.set_ui([this] {
            ui::next_window_place({0.0f, 0.0f}, {900.0f, 500.0f});
            console->draw("Console");
        });
        console->show_problems();
        h.step(3);
    }

    [[nodiscard]] bool shows(const std::string& message) const {
        return h.find(message, core::Role::ListItem) != nullptr;
    }
};

// The entries of the menu open, top to bottom. A kit menu entry records no
// label of its own, only an unlabelled line in the popup.
std::vector<core::Rect> menu_entries(KitHarness& h) {
    std::vector<core::Rect> entries;
    const auto popup = h.popup();
    if (!popup) return entries;
    for (const core::SemanticNode& node : h.ui().semantics()) {
        if (node.role != core::Role::ListItem || !node.label.empty()) continue;
        if (popup->contains((node.rect.min + node.rect.max) * 0.5f)) entries.push_back(node.rect);
    }
    std::ranges::sort(entries, {}, [](const core::Rect& rect) { return rect.min.y; });
    return entries;
}

// A console whose log holds `count` lines, "line 0" on, every third of two
// lines, the log shown.
struct LogConsole {
    KitHarness h{{900.0f, 500.0f}};
    ftk::ProblemList list;
    std::shared_ptr<ftk::ConsoleSink> console = std::make_shared<ftk::ConsoleSink>(list);

    explicit LogConsole(int count) {
        console->set_pattern("%v");
        spdlog::logger logger("log test", console);
        for (int i = 0; i < count; ++i) {
            if (i % 3 == 2) {
                logger.info("line {}\ncontinued", i);
            } else {
                logger.info("line {}", i);
            }
        }
        h.set_ui([this] {
            ui::next_window_place({0.0f, 0.0f}, {900.0f, 500.0f});
            console->draw("Console");
        });
        h.step(3);
    }

    // The log lines the last frame drew, by number.
    [[nodiscard]] std::map<int, core::Rect> drawn() {
        std::map<int, core::Rect> lines;
        for (const core::SemanticNode& node : h.ui().semantics()) {
            if (node.role != core::Role::ListItem || !node.label.starts_with("line ")) continue;
            lines[std::stoi(std::string(node.label.substr(5)))] = node.rect;
        }
        return lines;
    }
};

// Each line drawn sits under the one before by the same gap, whether that
// one is one line of text or two.
void check_one_after_another(const std::map<int, core::Rect>& lines) {
    REQUIRE(lines.size() > 2);
    const float gap = std::next(lines.begin())->second.min.y - lines.begin()->second.max.y;
    CHECK(gap >= 0.0f);
    for (auto line = std::next(lines.begin()); line != lines.end(); ++line) {
        const auto before = std::prev(line);
        INFO("line " << line->first);
        REQUIRE(line->first == before->first + 1);
        CHECK(std::abs(line->second.min.y - before->second.max.y - gap) < 0.5f);
    }
}

} // namespace

TEST_CASE("The Console's log draws only the lines in sight, one under another", "[ui][kit][console]") {
    LogConsole c(2000);
    const auto lines = c.drawn();
    CHECK(lines.contains(0));
    CHECK(lines.size() < 60);
    check_one_after_another(lines);
}

TEST_CASE("Scrolled to the end, the Console's log shows its last lines", "[ui][kit][console]") {
    LogConsole c(2000);
    c.h.wheel({450.0f, 300.0f}, -100000.0f);
    c.h.step(60);
    const auto lines = c.drawn();
    CHECK(lines.contains(1999));
    CHECK(lines.size() < 60);
    check_one_after_another(lines);
}

TEST_CASE("The Console's problems list errors first, the newest first, with the count on the switch",
          "[ui][kit][console]") {
    Console c;
    REQUIRE(c.h.find("Problems 3", core::Role::Button));
    const float font = c.h.rect("Font didn't bake: file not found").min.y;
    const float shader = c.h.rect("'foam_bias' : undeclared identifier").min.y;
    const float sheet = c.h.rect("Unknown property 'text-shadow'").min.y;
    CHECK(font < shader);
    CHECK(shader < sheet);
}

TEST_CASE("A fixed problem leaves the Console's list", "[ui][kit][console]") {
    Console c;
    c.list.clear("font:title");
    c.h.step(2);
    CHECK_FALSE(c.shows("Font didn't bake: file not found"));
    CHECK(c.shows("'foam_bias' : undeclared identifier"));
    CHECK(c.h.find("Problems 2", core::Role::Button));

    c.list.clear_all();
    c.h.step(2);
    CHECK_FALSE(c.shows("'foam_bias' : undeclared identifier"));
    CHECK_FALSE(c.shows("Unknown property 'text-shadow'"));
    CHECK(c.h.find("Problems", core::Role::Button));
}

TEST_CASE("The severity toggles and the search narrow the Console's problems", "[ui][kit][console]") {
    Console c;
    c.h.click("Warnings");
    c.h.step(2);
    CHECK_FALSE(c.shows("Unknown property 'text-shadow'"));
    CHECK(c.shows("'foam_bias' : undeclared identifier"));
    c.h.click("Warnings");
    c.h.step(2);
    CHECK(c.shows("Unknown property 'text-shadow'"));

    const core::SemanticNode* search = c.h.find("", core::Role::TextField);
    REQUIRE(search);
    c.h.click_at((search->rect.min + search->rect.max) * 0.5f);
    c.h.type("foam");
    c.h.step(2);
    CHECK(c.shows("'foam_bias' : undeclared identifier"));
    CHECK_FALSE(c.shows("Font didn't bake: file not found"));
    CHECK_FALSE(c.shows("Unknown property 'text-shadow'"));
}

TEST_CASE("Double-clicking a problem opens its line, or shows a file without one", "[ui][kit][console]") {
    Console c;
    c.h.double_click("'foam_bias' : undeclared identifier");
    c.h.step();
    REQUIRE(c.opened.size() == 1);
    CHECK(c.opened[0].file == SHADER);
    CHECK(c.opened[0].line == 41);
    CHECK(c.revealed.empty());

    c.h.double_click("Font didn't bake: file not found");
    c.h.step();
    REQUIRE(c.revealed.size() == 1);
    CHECK(c.revealed[0].file == "/project/fonts/Title.fjfont");
    CHECK(c.opened.size() == 1);
}

TEST_CASE("A problem's menu opens, shows and copies it", "[ui][kit][console]") {
    Console c;
    // Open, Show in content browser, Copy.
    c.h.click("'foam_bias' : undeclared identifier", ui::MouseButton::Right);
    c.h.step();
    std::vector<core::Rect> entries = menu_entries(c.h);
    REQUIRE(entries.size() == 3);
    c.h.click_at((entries[0].min + entries[0].max) * 0.5f);
    c.h.step();
    REQUIRE(c.opened.size() == 1);
    CHECK(c.opened[0].line == 41);

    c.h.click("'foam_bias' : undeclared identifier", ui::MouseButton::Right);
    c.h.step();
    entries = menu_entries(c.h);
    REQUIRE(entries.size() == 3);
    c.h.click_at((entries[1].min + entries[1].max) * 0.5f);
    c.h.step();
    REQUIRE(c.revealed.size() == 1);
    CHECK(c.revealed[0].line == 41);

    c.h.click("'foam_bias' : undeclared identifier", ui::MouseButton::Right);
    c.h.step();
    entries = menu_entries(c.h);
    REQUIRE(entries.size() == 3);
    c.h.click_at((entries[2].min + entries[2].max) * 0.5f);
    c.h.step();
    CHECK(c.h.ui().clipboard() ==
          SHADER + ":41: 'foam_bias' : undeclared identifier (It draws with the last version that compiled)");
}

TEST_CASE("Opening a problem may log through the Console it was opened from", "[ui][kit][console]") {
    Console c;
    // The console holds its lock while it draws; an action that logs runs
    // once it has let go, or this would never return.
    auto logger = std::make_shared<spdlog::logger>("console_test", c.console);
    int logged = 0;
    c.console->set_problem_actions({.open = [&](const ftk::Problem& p) {
        logger->error("Couldn't open {}", p.file);
        ++logged;
    }});
    c.h.double_click("'foam_bias' : undeclared identifier");
    c.h.step();
    CHECK(logged == 1);
}
