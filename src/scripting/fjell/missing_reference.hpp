// missing_reference.hpp — what a Ref or an Asset used with nothing there throws
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include <exception>

namespace fjell {

/// Thrown by `->` and `*` on a Ref whose target is gone, and by an SDK call
/// handed an Asset that is unset or whose file failed to load. The engine has
/// already been told by then, and stops the game with the script, the field,
/// the node and the hook named; the throw only ends what the script was doing.
/// Catching it does not keep the game running.
struct MissingReference : std::exception {
    [[nodiscard]] const char* what() const noexcept override {
        return "a reference was used with nothing there";
    }
};

} // namespace fjell
