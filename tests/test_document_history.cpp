#include "ftk/app/document_history.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using ftk::CommandHistory;
using ftk::DocumentHistory;
using ftk::Result;

namespace {

// An editor whose document is one string it edits directly.
struct Editor {
    CommandHistory history;
    std::string text = "mass=1";
    int restores = 0;
    DocumentHistory undo{history, [this] { return text; },
                         [this](const std::string& t) -> Result<> { text = t; ++restores; return {}; }};
};

} // namespace

TEST_CASE("A finished direct edit becomes one undo step, and undo puts the text back", "[ui][document]") {
    Editor e;
    e.undo.opened();

    // Dragging: nothing is recorded until the drag lets go.
    e.text = "mass=2";
    e.undo.frame(true, false, "Mass");
    e.text = "mass=3";
    e.undo.frame(true, false, "Mass");
    CHECK(e.history.commands().empty());
    e.undo.frame(false, false, "Mass: 1 -> 3");
    REQUIRE(e.history.commands().size() == 1);
    CHECK(e.history.commands()[0]->description() == "Mass: 1 -> 3");

    e.history.undo();
    CHECK(e.text == "mass=1");
    e.history.redo();
    CHECK(e.text == "mass=3");
}

TEST_CASE("Undoing is not itself recorded as an edit", "[ui][document]") {
    Editor e;
    e.undo.opened();
    e.text = "mass=2";
    e.undo.frame(false, true, "Mass");
    e.history.undo();
    // The frame after the undo sees a changed document and a moved history.
    e.undo.frame(false, true, "Ctrl Z");
    CHECK(e.history.commands().size() == 1);
    CHECK(e.history.current_index() == -1);
}

TEST_CASE("A frame with no finished input records nothing, and no change records nothing", "[ui][document]") {
    Editor e;
    e.undo.opened();
    e.text = "mass=2";
    e.undo.frame(false, false, "Mass");
    CHECK(e.history.commands().empty());
    e.undo.frame(false, true, "Mass");
    CHECK(e.history.commands().size() == 1);
    e.undo.frame(false, true, "Mass");
    CHECK(e.history.commands().size() == 1);
}

TEST_CASE("A step the editor records itself is followed, not recorded twice", "[ui][document]") {
    struct SetText final : ftk::Command {
        std::string& text;
        std::string before;
        std::string after;
        SetText(std::string& t, std::string a) : text{t}, before{t}, after{std::move(a)} {}
        void execute() override { text = after; }
        void undo() override { text = before; }
        [[nodiscard]] std::string description() const override { return "Set"; }
    };
    Editor e;
    e.undo.opened();
    e.history.execute(std::make_unique<SetText>(e.text, "mass=5"));
    e.undo.frame(false, true, "Mass");
    CHECK(e.history.commands().size() == 1);
    CHECK(e.restores == 0);
}
