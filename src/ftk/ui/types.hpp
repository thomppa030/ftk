#pragma once

// The kit's own types where its headers took ImGui's, so a program reaches
// the kit without ImGui. Points and sizes are glm::vec2. Those the UI core
// shares (a colour, an id, a texture, a payload) are the core's; the font
// is the kit's, while ImGui draws it.

#include "ftk/ui/core/types.hpp"

// ImGui's font, which a Font is while ImGui draws the kit. Named here, not
// included: the bridge goes with ImGui.
struct ImFont;

namespace ftk::ui {

/// One of the theme's faces, at no size of its own: the size is given where
/// it is drawn. While ImGui draws the kit it is one of ImGui's fonts, and
/// goes wherever ImGui takes one.
class Font {
public:
    constexpr Font() = default;
    constexpr Font(ImFont* font) : font_{font} {}

    constexpr operator ImFont*() const { return font_; }

private:
    ImFont* font_{nullptr};
};

} // namespace ftk::ui
