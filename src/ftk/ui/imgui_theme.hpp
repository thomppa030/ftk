#pragma once

// The theme as ImGui takes it: its fonts loaded into a context's atlas, and
// its colours and metrics written into ImGui's style. For what runs ImGui
// (the ImGui layer, the test harness), not for widgets, which take the theme
// through ftk/ui/theme.hpp.

#include <string>

struct ImFont;
struct ImGuiContext;
struct ImGuiStyle;

namespace ftk::theme {

/// Load editor fonts (Geist preferred, Inter as fallback) with Lucide icons
/// merged into the default + bold weights. Call after ImGui::CreateContext()
/// but before backend init. Returns true if at least the default font loaded.
bool load_font(const std::string& font_dir);

/// Forgets the faces of a context about to be destroyed, so a new context
/// at the same address doesn't find them.
void forget_fonts(const ImGuiContext* context);

/// Writes the theme's colours and metrics into `style`.
void apply(ImGuiStyle& style);

namespace detail {

/// The faces load_font() found for one context. Either is null when its file
/// was missing.
struct Faces {
    ImFont* bold{nullptr};
    ImFont* mono{nullptr};
};

/// The current context's faces.
Faces& current_faces();

} // namespace detail

} // namespace ftk::theme
