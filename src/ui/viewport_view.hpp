#pragma once

#include "renderer/viewport_source.hpp"

// What a viewport shows. Each viewport picks its own buffer (the lit
// picture, depth, normals…); the lit picture can be replaced by one
// lighting term or a DDGI view, which the renderer applies to every
// viewport at once. One menu picks any of them, on the viewport's toolbar.

namespace fjell {

class VulkanContext;

/// Whether the viewport shows the ordinary lit picture: its own buffer is
/// the lit one and no lighting channel or DDGI view replaces it.
[[nodiscard]] bool view_is_lit(ViewportSource source, VulkanContext& ctx);

/// The name of what the viewport shows: "Lit", "Depth", "DDGI indirect".
[[nodiscard]] const char* view_name(ViewportSource source, VulkanContext& ctx);

/// The entries of the view menu, grouped and with the current one
/// checked. Picking one sets the viewport's `source`, or the renderer's
/// lighting channel and DDGI view. Drawn inside an open popup.
void view_menu(ViewportSource& source, VulkanContext& ctx);

} // namespace fjell
