#pragma once

#include "ftk/app/command.hpp"

#include <string>
#include <vector>

namespace ftk {

class CommandHistory {
public:
    void execute(CommandPtr cmd);
    /// Adds a step that has already been carried out, without carrying it
    /// out again: an edit made directly and recorded after the fact.
    void record(CommandPtr cmd);
    void undo();
    void redo();
    void clear();
    void jump_to(int index);

    [[nodiscard]] const std::vector<CommandPtr>& commands() const { return commands_; }
    [[nodiscard]] int current_index() const { return current_; }
    [[nodiscard]] bool can_undo() const { return current_ >= 0; }
    [[nodiscard]] bool can_redo() const { return current_ < static_cast<int>(commands_.size()) - 1; }

    /// Where it stands now is what was saved: what changes_since_saved()
    /// reads from.
    void mark_saved();
    /// What changed between the saved state and now, oldest first: the steps
    /// done since, and those undone past it ("Undone: Add Oak 4"), as a
    /// ChangeList says them: steps on one value merge into one, saved -> now,
    /// and drop out once it is back where it was saved.
    [[nodiscard]] std::vector<std::string> changes_since_saved() const;

private:
    /// Takes away the steps undone, before a new one: the way back to the
    /// saved state among them is kept as words, since no undo reaches it.
    void forget_undone();

    std::vector<CommandPtr> commands_;
    int current_{-1};
    /// The saved state: the step it stood at, then the steps done after that
    /// one which were undone and replaced since.
    int saved_{-1};
    std::vector<std::string> beyond_saved_;
};

} // namespace ftk
