#pragma once

#include "ftk/ui/kit/edit.hpp"
#include "physics/collision_channel.hpp"

namespace fjell {

/// A collision filter in a component's inspector: the channel the collider
/// belongs to, then one row per channel saying what it does on meeting it.
/// Shared by every component that carries a filter, so a collider and a
/// character present collision the same way.
ui::Edit draw_collision_filter(CollisionFilter& filter);

} // namespace fjell
