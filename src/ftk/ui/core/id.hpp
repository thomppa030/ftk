#pragma once

// Widget ids (decision 5 of the UI core plan): a label hashed onto the ids
// pushed around it, so the same label in two places is two widgets, and a
// widget keeps its id from frame to frame without being kept anywhere.
//
//     "Save"          shown as Save, its id from "Save"
//     "Save##scene"   shown as Save, its id from all of it: a second Save
//     "Play###state"  shown as Play, its id from "###state" alone, so it
//                     keeps its id when the label changes to Pause
//
//     IdStack ids;
//     ids.push("Layers");
//     const Id opacity = ids.id("Opacity");  // told apart from another panel's
//     ids.pop();

#include "ftk/ui/core/types.hpp"

#include <string_view>
#include <vector>

namespace ftk::ui::core {

/// `label` hashed onto `seed`, by the rules above. Never zero, which no
/// widget has.
[[nodiscard]] Id hash_label(std::string_view label, Id seed);
/// A number or an address hashed onto `seed`: an element of a list told
/// apart by its index or by the object it shows.
[[nodiscard]] Id hash_int(int value, Id seed);
[[nodiscard]] Id hash_pointer(const void* pointer, Id seed);

/// The part of `label` that is shown: what comes before "##".
[[nodiscard]] std::string_view shown_label(std::string_view label);

/// The ids pushed around what is being drawn, innermost on top.
class IdStack {
public:
    IdStack() = default;

    /// `label`'s id under the ids pushed now. A string literal is a label,
    /// not an address: without its own overload it would take the pointer's.
    [[nodiscard]] Id id(std::string_view label) const { return hash_label(label, top()); }
    [[nodiscard]] Id id(const char* label) const { return hash_label(label, top()); }
    [[nodiscard]] Id id(int value) const { return hash_int(value, top()); }
    [[nodiscard]] Id id(const void* pointer) const { return hash_pointer(pointer, top()); }

    /// What is drawn next is told apart by `label` (or the number, the
    /// address, or an id already hashed) as well, until the matching pop().
    void push(std::string_view label) { stack_.push_back(id(label)); }
    void push(const char* label) { stack_.push_back(id(label)); }
    void push(int value) { stack_.push_back(id(value)); }
    void push(const void* pointer) { stack_.push_back(id(pointer)); }
    void push_id(Id id) { stack_.push_back(id); }
    void pop() { stack_.pop_back(); }

    /// The innermost id pushed, or zero at the bottom.
    [[nodiscard]] Id top() const { return stack_.empty() ? 0 : stack_.back(); }
    [[nodiscard]] std::size_t depth() const { return stack_.size(); }
    void clear() { stack_.clear(); }

private:
    std::vector<Id> stack_;
};

} // namespace ftk::ui::core
