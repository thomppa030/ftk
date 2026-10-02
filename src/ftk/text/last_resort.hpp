#pragma once

// The face of last resort: Unicode's Last Resort font, in its compact form,
// compiled in. It has a glyph for every character, showing the script or
// block the character belongs to, so text a system can't draw still says
// what is missing ("Japanese") rather than leaving boxes. It goes last in a
// font chain, after the system's fonts.
//
//     text::FontChain chain({inter}, system, text::last_resort());

#include "ftk/text/face.hpp"

#include <memory>

namespace ftk::text {

/// Loaded on the first call and shared; null only if the compiled-in font
/// can't be read, which a test guards against.
[[nodiscard]] std::shared_ptr<const Face> last_resort();

} // namespace ftk::text
