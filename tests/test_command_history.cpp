#include "ftk/app/change_list.hpp"
#include "ftk/app/command_history.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using ftk::CommandHistory;
using ftk::describe_change;

namespace {

// A step that does nothing but name itself.
class Step : public ftk::Command {
public:
    explicit Step(std::string description) : description_{std::move(description)} {}
    void execute() override {}
    void undo() override {}
    [[nodiscard]] std::string description() const override { return description_; }

private:
    std::string description_;
};

void step(CommandHistory& history, const std::string& description) {
    history.execute(std::make_unique<Step>(description));
}

std::string mass(const char* before, const char* after) { return describe_change("Crate: Mass", before, after); }

} // namespace

TEST_CASE("A change is read back as describe_change wrote it", "[app][history]") {
    const auto change = ftk::read_change(mass("1 kg", "2 kg"));
    REQUIRE(change);
    CHECK(change->what == "Crate: Mass");
    CHECK(change->before == "1 kg");
    CHECK(change->after == "2 kg");
    CHECK_FALSE(ftk::read_change("Add Oak 4"));
}

TEST_CASE("A history says what changed since it was saved, a value once as saved to now", "[app][history]") {
    CommandHistory history;
    step(history, "Add Oak 4");
    history.mark_saved();
    CHECK(history.changes_since_saved().empty());

    step(history, mass("1 kg", "2 kg"));
    step(history, "Delete Rock 3");
    step(history, mass("2 kg", "3 kg"));
    CHECK(history.changes_since_saved() == std::vector<std::string>{mass("1 kg", "3 kg"), "Delete Rock 3"});

    // Back where it was saved, the value says nothing.
    step(history, mass("3 kg", "1 kg"));
    CHECK(history.changes_since_saved() == std::vector<std::string>{"Delete Rock 3"});

    // Nothing marked saved: everything it holds.
    CommandHistory fresh;
    step(fresh, "Add Oak 4");
    CHECK(fresh.changes_since_saved() == std::vector<std::string>{"Add Oak 4"});
}

TEST_CASE("A history undone past its save says what was undone, even once replaced", "[app][history]") {
    CommandHistory history;
    step(history, "Add Oak 4");
    step(history, mass("1 kg", "2 kg"));
    history.mark_saved();

    history.undo();
    history.undo();
    CHECK(history.changes_since_saved() == std::vector<std::string>{mass("2 kg", "1 kg"), "Undone: Add Oak 4"});

    // A new step takes the undone ones away; the way back to the save is kept.
    step(history, "Delete Rock 3");
    CHECK(history.changes_since_saved() ==
          std::vector<std::string>{mass("2 kg", "1 kg"), "Undone: Add Oak 4", "Delete Rock 3"});
    history.undo();
    CHECK(history.changes_since_saved() == std::vector<std::string>{mass("2 kg", "1 kg"), "Undone: Add Oak 4"});

    // Saved again, it starts over.
    history.mark_saved();
    CHECK(history.changes_since_saved().empty());
}

TEST_CASE("A change list says each value once, from where it started", "[app][history]") {
    ftk::ChangeList list;
    list.add(describe_change("Rendering: Distance", "120", "200"));
    list.add(describe_change("Wind: Gust strength", "0.4", "0.6"));
    list.add(describe_change("Wind: Gust strength", "0.6", "0.8"));
    list.add(describe_change("Rendering: Distance", "200", "120"));
    CHECK(list.lines() == std::vector<std::string>{describe_change("Wind: Gust strength", "0.4", "0.8")});
    list.add("Add Oak 4", true);
    CHECK(list.lines().back() == "Undone: Add Oak 4");
    list.clear();
    CHECK(list.lines().empty());
}
