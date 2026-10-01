#pragma once

namespace fjell {

/// Registers the engine's own file types with the kit (ftk::ui::asset_kind()),
/// so every list, picker and slot shows a .fjmat as a material and a .fjell as
/// a scene. Once, before the editor first draws; calling it again changes
/// nothing.
void register_asset_kinds();

} // namespace fjell
