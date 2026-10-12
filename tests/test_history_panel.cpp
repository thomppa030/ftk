#include "kit_harness.hpp"

#include "ftk/app/command.hpp"
#include "ftk/app/command_history.hpp"
#include "ftk/app/history_panel.hpp"
#include "ftk/ui/kit/window.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>

using ftk::test::KitHarness;
namespace ui = ftk::ui;
namespace core = ftk::ui::core;

namespace {

// A step that changes nothing, described as it is told.
class Step : public ftk::Command {
public:
    explicit Step(std::string text) : text_{std::move(text)} {}
    void execute() override {}
    void undo() override {}
    [[nodiscard]] std::string description() const override { return text_; }

private:
    std::string text_;
};

// A History panel over `count` steps, "step 0" on, every one done.
struct History {
    KitHarness h{{600.0f, 500.0f}};
    ftk::CommandHistory history;
    ftk::HistoryPanel panel;

    explicit History(int count) {
        for (int i = 0; i < count; ++i) history.record(std::make_unique<Step>("step " + std::to_string(i)));
        panel.init(&history);
        h.set_ui([this] {
            ui::next_window_place({0.0f, 0.0f}, {600.0f, 500.0f});
            panel.draw();
        });
        h.step(3);
    }

    // The steps the last frame drew, by number.
    [[nodiscard]] std::map<int, core::Rect> drawn() {
        std::map<int, core::Rect> steps;
        for (const core::SemanticNode& node : h.ui().semantics()) {
            if (node.role != core::Role::ListItem || !node.label.starts_with("step ")) continue;
            steps[std::stoi(std::string(node.label.substr(5)))] = node.rect;
        }
        return steps;
    }

    void scroll_to_top() {
        h.wheel({300.0f, 250.0f}, 100000.0f);
        h.step(60);
    }
};

// Each step drawn sits under the one before by the same gap.
void check_one_after_another(const std::map<int, core::Rect>& steps) {
    REQUIRE(steps.size() > 2);
    const float gap = std::next(steps.begin())->second.min.y - steps.begin()->second.max.y;
    CHECK(gap >= 0.0f);
    for (auto step = std::next(steps.begin()); step != steps.end(); ++step) {
        const auto before = std::prev(step);
        INFO("step " << step->first);
        REQUIRE(step->first == before->first + 1);
        CHECK(std::abs(step->second.min.y - before->second.max.y - gap) < 0.5f);
    }
}

} // namespace

TEST_CASE("The History panel draws only the steps in sight, one under another", "[ui][kit][history]") {
    History c(2000);
    const auto steps = c.drawn();
    // Following the newest step, as it does while scrolled to the end.
    CHECK(steps.contains(1999));
    CHECK(steps.size() < 60);
    check_one_after_another(steps);
}

TEST_CASE("Scrolled to the top, the History panel shows its initial state and first steps", "[ui][kit][history]") {
    History c(2000);
    c.scroll_to_top();
    const auto steps = c.drawn();
    CHECK(c.h.find("Initial state", core::Role::ListItem) != nullptr);
    CHECK(steps.contains(0));
    CHECK(steps.size() < 60);
    check_one_after_another(steps);
    // The initial state sits above the first step by the gap between steps.
    const float gap = std::next(steps.begin())->second.min.y - steps.begin()->second.max.y;
    CHECK(std::abs(steps.at(0).min.y - c.h.rect("Initial state").max.y - gap) < 0.5f);
}

TEST_CASE("A step clicked in the History panel is gone back to, and the initial state undoes them all",
          "[ui][kit][history]") {
    History c(2000);
    c.scroll_to_top();
    c.h.click("step 3");
    CHECK(c.history.current_index() == 3);
    c.h.click("Initial state");
    CHECK(c.history.current_index() == -1);
}
