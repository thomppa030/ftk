#pragma once

#include "ftk/ui/theme.hpp"

// The heading of one thing on a node, a component or a script: a bar in
// its hue over a framed header with the fold chevron, the name
// and a trash icon that removes it. The middle of the three heading levels
// (ftk/ui/kit/section.hpp has the other two).

namespace ftk::ui {

/// Draws the block's heading and returns whether it is open; the caller
/// draws the body only then. `remove` is set when the trash icon is
/// clicked, and removal is the caller's to do, undoably. The fold state is
/// kept under the current ID, so each block needs its own ID scope.
bool component_block(const char* name, theme::Hue hue, bool& remove);

/// The same with an on/off switch before the trash icon, for something that
/// can be switched off and keep its settings (a VFX module). Off, the bar
/// goes grey and the name dims; draw the body in a FadedBody.
bool component_block(const char* name, theme::Hue hue, bool& remove, bool& enabled);

/// While it lives, what is drawn is faded: the body of a block switched off,
/// still editable so it can be set up before it is switched on.
class FadedBody {
public:
    explicit FadedBody(bool faded);
    ~FadedBody();
    FadedBody(const FadedBody&) = delete;
    FadedBody& operator=(const FadedBody&) = delete;
    FadedBody(FadedBody&&) = delete;
    FadedBody& operator=(FadedBody&&) = delete;

private:
    bool faded_;
};

} // namespace ftk::ui
