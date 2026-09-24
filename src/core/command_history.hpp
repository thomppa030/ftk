#pragma once

#include "core/command.hpp"

#include <vector>

namespace fjell {

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

private:
    std::vector<CommandPtr> commands_;
    int current_{-1};
};

} // namespace fjell
