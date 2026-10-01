#include "ftk/app/command.hpp"
#include "ftk/app/command_history.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

// Appends its letter when carried out and takes it off when undone, so the
// log shows the order things happened in.
class Step : public ftk::Command {
public:
    Step(std::string& log, char letter) : log_{log}, letter_{letter} {}
    void execute() override { log_ += letter_; }
    void undo() override { log_ += static_cast<char>(letter_ - 'a' + 'A'); }
    [[nodiscard]] std::string description() const override { return std::string(1, letter_); }

private:
    std::string& log_;
    char letter_;
};

} // namespace

TEST_CASE("A command group is one undo step whose commands run in order and undo backwards", "[commands]") {
    std::string log;
    ftk::CommandHistory history;
    auto group = std::make_unique<ftk::CommandGroup>("Delete 2 objects");
    group->add(std::make_unique<Step>(log, 'a'));
    group->add(std::make_unique<Step>(log, 'b'));
    // Carried out as they were added, each seeing what the one before left.
    CHECK(log == "ab");

    history.execute(std::move(group));
    // Recording the group does not carry the commands out a second time.
    CHECK(log == "ab");

    history.undo();
    CHECK(log == "abBA");
    history.redo();
    CHECK(log == "abBAab");
}
