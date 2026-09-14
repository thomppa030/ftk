#pragma once

#include "physics/collision_channel.hpp"

namespace fjell {

// Inspector widget for a collision filter: channel picker plus the
// per-channel response table. Shared by every component that carries one, so
// a collider and a character present collision the same way. Returns true
// when the filter changed.
bool draw_collision_filter(CollisionFilter& filter);

} // namespace fjell
