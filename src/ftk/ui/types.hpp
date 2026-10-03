#pragma once

// The kit's own types. Points and sizes are glm::vec2. Those the UI core
// shares (a colour, an id, a texture, a payload) are the core's; the font
// is the kit's.

#include "ftk/ui/core/types.hpp"

#include <cstdint>

namespace ftk::ui {

/// One of the theme's faces, by what it is for, at no size of its own: the
/// size is given where it is drawn. Each implementation of the kit finds its
/// own font for a face; `Current` keeps the face text is drawn in now.
enum class Font : uint8_t {
    Current,
    Body,
    /// For headings.
    Bold,
    /// For the console and code.
    Mono,
};

} // namespace ftk::ui
