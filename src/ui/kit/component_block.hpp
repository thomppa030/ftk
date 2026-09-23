#pragma once

#include "ui/theme.hpp"

// The heading of one thing on a node, a component or a script: a bar in
// its category colour over a framed header with the fold chevron, the name
// and a trash icon that removes it. The middle of the three heading levels
// (ui/kit/section.hpp has the other two).

namespace fjell::ui {

/// Draws the block's heading and returns whether it is open; the caller
/// draws the body only then. `remove` is set when the trash icon is
/// clicked, and removal is the caller's to do, undoably. The fold state is
/// kept under the current ID, so each block needs its own ID scope.
bool component_block(const char* name, theme::Category category, bool& remove);

} // namespace fjell::ui
