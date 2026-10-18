#include "ftk/app/command_history.hpp"

#include "ftk/app/change_list.hpp"

#include <utility>

namespace ftk {

void CommandHistory::forget_undone() {
    if (saved_ > current_) {
        std::vector<std::string> undone;
        for (int i = current_ + 1; i <= saved_; ++i) undone.push_back(commands_[static_cast<std::size_t>(i)]->description());
        beyond_saved_.insert(beyond_saved_.begin(), undone.begin(), undone.end());
        saved_ = current_;
    }
    // Parenthesise to avoid UB: (begin() + negative) is invalid on MSVC debug iterators
    commands_.erase(commands_.begin() + (current_ + 1), commands_.end());
}

void CommandHistory::execute(CommandPtr cmd) {
    forget_undone();
    commands_.push_back(std::move(cmd));
    current_ = static_cast<int>(commands_.size()) - 1;
    commands_[current_]->execute();
}

void CommandHistory::record(CommandPtr cmd) {
    forget_undone();
    commands_.push_back(std::move(cmd));
    current_ = static_cast<int>(commands_.size()) - 1;
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
    saved_ = -1;
    beyond_saved_.clear();
}

void CommandHistory::mark_saved() {
    saved_ = current_;
    beyond_saved_.clear();
}

std::vector<std::string> CommandHistory::changes_since_saved() const {
    // The way from the saved state to now: the steps beyond it undone, then
    // back down to where it stands or on up to it.
    std::vector<std::pair<std::string, bool>> way;
    for (auto step = beyond_saved_.rbegin(); step != beyond_saved_.rend(); ++step) way.emplace_back(*step, true);
    for (int i = saved_; i > current_; --i) way.emplace_back(commands_[static_cast<std::size_t>(i)]->description(), true);
    for (int i = saved_ + 1; i <= current_; ++i) way.emplace_back(commands_[static_cast<std::size_t>(i)]->description(), false);

    ChangeList changes;
    for (const auto& [description, undone] : way) changes.add(description, undone);
    return changes.lines();
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

} // namespace ftk
