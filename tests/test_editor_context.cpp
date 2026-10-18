#include "kit_harness.hpp"

#include "ftk/app/command.hpp"
#include "ftk/app/editor_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using ftk::test::KitHarness;

namespace {

// A step that changes nothing, described as it is told.
class Step : public ftk::Command {
public:
    explicit Step(std::string description) : description_{std::move(description)} {}
    void execute() override {}
    void undo() override {}
    [[nodiscard]] std::string description() const override { return description_; }

private:
    std::string description_;
};

// An editor of one line of text, opened when told.
class Notes : public ftk::EditorContext {
public:
    std::string text = "a";

    [[nodiscard]] const char* name() const override { return "Notes"; }
    [[nodiscard]] const char* context_type() const override { return "notes"; }
    void init() override {}
    void draw(float /*dt*/) override {}
    bool handle_shortcuts() override { return false; }
    [[nodiscard]] std::string document() const override { return text; }
    [[nodiscard]] ftk::Result<> restore(const std::string& with) override {
        text = with;
        return {};
    }
    void open() { document_opened(); }
    void edit(const std::string& description, std::string to) {
        text = std::move(to);
        command_history().record(std::make_unique<Step>(description));
    }
};

} // namespace

TEST_CASE("An editor's changes count from where it was opened or last saved", "[app][history]") {
    KitHarness h;
    Notes notes;
    notes.edit("Add Oak 4", "b");
    notes.open();
    CHECK(notes.changes().empty());

    notes.edit("Delete Rock 3", "c");
    CHECK(notes.changes() == std::vector<std::string>{"Delete Rock 3"});
    REQUIRE(notes.save());
    CHECK(notes.changes().empty());
}
