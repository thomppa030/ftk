#pragma once

#include "ftk/app/command.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ftk {

/// What changed, a line each, as the unsaved list says it. Steps are given
/// in the order they happened: those on one value (describe_change()) merge
/// into one line, its first before -> its last after, left out once it is
/// back where it started; any other step is a line of its own.
class ChangeList {
public:
    /// `description` done, or undone: an undone value goes back from its
    /// after to its before, and any other undone step reads "Undone: ...".
    void add(const std::string& description, bool undone = false);
    void clear() { lines_.clear(); }
    /// The lines, in the order each first changed.
    [[nodiscard]] std::vector<std::string> lines() const;

private:
    struct Line {
        std::optional<ValueChange> value;
        std::string text;
    };
    std::vector<Line> lines_;
};

} // namespace ftk
