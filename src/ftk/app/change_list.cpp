#include "ftk/app/change_list.hpp"

#include <algorithm>
#include <utility>

namespace ftk {

void ChangeList::add(const std::string& description, bool undone) {
    auto change = read_change(description);
    if (!change) {
        lines_.push_back({.value = std::nullopt, .text = undone ? "Undone: " + description : description});
        return;
    }
    if (undone) std::swap(change->before, change->after);
    const auto same = std::ranges::find_if(lines_, [&](const Line& line) { return line.value && line.value->what == change->what; });
    if (same != lines_.end()) {
        same->value->after = change->after;
    } else {
        lines_.push_back({.value = std::move(change), .text = {}});
    }
}

std::vector<std::string> ChangeList::lines() const {
    std::vector<std::string> said;
    for (const Line& line : lines_) {
        if (!line.value) {
            said.push_back(line.text);
        } else if (line.value->before != line.value->after) {
            said.push_back(describe_change(line.value->what, line.value->before, line.value->after));
        }
    }
    return said;
}

} // namespace ftk
