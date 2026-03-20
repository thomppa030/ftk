#include "core/command_history.hpp"

namespace fjell {

void CommandHistory::execute(CommandPtr cmd) {
    // Truncate any undone commands
    // Parenthesise to avoid UB: (begin() + negative) is invalid on MSVC debug iterators
    commands_.erase(commands_.begin() + (current_ + 1), commands_.end());
    commands_.push_back(std::move(cmd));
    current_ = static_cast<int>(commands_.size()) - 1;
    commands_[current_]->execute();
}

void CommandHistory::undo() {
    if (!can_undo()) return;
    commands_[current_]->undo();
    current_--;
}

void CommandHistory::redo() {
    if (!can_redo()) return;
    current_++;
    commands_[current_]->execute();
}

void CommandHistory::clear() {
    commands_.clear();
    current_ = -1;
}

void CommandHistory::jump_to(int index) {
    if (index < -1 || index >= static_cast<int>(commands_.size())) return;

    while (current_ > index) {
        undo();
    }
    while (current_ < index) {
        redo();
    }
}

} // namespace fjell
