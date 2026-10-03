#pragma once

// What a program drawing the kit does once, before the kit's first frame:
// give the kit its faces.
//
//     ui::use_kit_fonts(app.ui(), staged / "fonts");

#include "ftk/ui/core/context.hpp"

#include <filesystem>

namespace ftk::ui {

/// Adds the kit's faces from `directory` to `ui`'s text: Geist Medium for
/// the body, with lucide's icons, Geist Bold and Geist Mono, or Inter and
/// JetBrains Mono where Geist isn't there; each falls back to the system's
/// fonts and then to the last resort. The body is the text drawn when no
/// face is asked for. False, and logged, when the body's face can't be read.
bool use_kit_fonts(core::Context& ui, const std::filesystem::path& directory);

/// The kit's window `name` at `pos`, `size` big, with nothing of its own
/// but its background and padding: no title bar, not moved or resized,
/// behind the kit's other windows. Where a program's UI goes when it fills
/// the OS window, as a test's does in the harness. Ended by
/// end_plain_window() whatever it returns.
bool begin_plain_window(const char* name, glm::vec2 pos, glm::vec2 size);
void end_plain_window();

} // namespace ftk::ui
