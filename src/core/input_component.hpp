#pragma once

#include <string>
#include <vector>

namespace fjell {

/// Engine component that defines which InputMappingContexts are active for a node.
/// Attach to a node alongside ScriptComponents that have [[fj::action]] methods.
/// On play start, contexts are pushed to the InputActionMap stack.
/// On play stop, they are popped.
struct InputComponent {
    /// Paths to .fjinput context files, bottom-to-top stack order.
    std::vector<std::string> context_paths;

    /// Directory to scan for .fjaction files (resolved relative to project root).
    /// If empty, uses project-wide "assets/input/actions/" default.
    std::string actions_dir;
};

} // namespace fjell
