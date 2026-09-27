// sprite_anim_api.hpp — Sprite animation API for game scripts
#pragma once

#include "fjell/api.h"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void  fjell_sprite_anim_set_param(uint32_t node_id, const char* name, float value);
FJELL_SDK_EXPORT float fjell_sprite_anim_get_param(uint32_t node_id, const char* name);
FJELL_SDK_EXPORT const char* fjell_sprite_anim_current_state(uint32_t node_id);
FJELL_SDK_EXPORT void  fjell_sprite_anim_set_flip_h(uint32_t node_id, bool flip);
FJELL_SDK_EXPORT bool  fjell_sprite_anim_is_finished(uint32_t node_id);
FJELL_SDK_EXPORT void  fjell_sprite_anim_set_playing(uint32_t node_id, bool playing);
}

namespace fjell {

/// Drives 2D sprite animation on a node with a SpriteAnimComponent — the
/// sprite-sheet counterpart to `animator`. You set parameters; the animation
/// state machine decides which sheet plays.
///
/// Every call silently does nothing (or returns a default) when the node has
/// no SpriteAnimComponent.
///
/// @code
/// void tick(float dt) override {
///     fjell::sprite_anim::set_param(self().id(), "speed", std::abs(vel_.x));
///     fjell::sprite_anim::set_flip_h(self().id(), vel_.x < 0.0f);
/// }
/// @endcode
namespace sprite_anim {
/// Set a state machine parameter, driving transitions between sheets.
/// @param name  Parameter name as spelled in the state machine. Unknown
///              names are created rather than rejected.
/// @param value Booleans are passed as 0.0 / 1.0.
inline void set_param(uint32_t node_id, const char* name, float value) {
    fjell_sprite_anim_set_param(node_id, name, value);
}

/// Read back a state machine parameter.
/// @return The current value, or 0.0 if the node has no sprite animator or
///         the parameter was never set.
inline float get_param(uint32_t node_id, const char* name) {
    return fjell_sprite_anim_get_param(node_id, name);
}

/// Name of the animation state currently playing.
/// @return The state name, or "" if the node has no sprite animator.
/// @warning Points to engine-owned storage that changes on the next
///          transition. Compare it immediately; copy it if you need to keep it.
inline const char* current_state(uint32_t node_id) {
    return fjell_sprite_anim_current_state(node_id);
}

/// Mirror the sprite horizontally — the usual way to face a 2D character
/// left or right from one set of right-facing sheets.
inline void set_flip_h(uint32_t node_id, bool flip) {
    fjell_sprite_anim_set_flip_h(node_id, flip);
}

/// Whether a non-looping animation has reached its last frame. Stays true
/// until a different state begins, so it is safe to poll from `tick()`.
/// @return False for looping animations, and for a node with no sprite
///         animator.
inline bool is_finished(uint32_t node_id) {
    return fjell_sprite_anim_is_finished(node_id);
}

/// Pause or resume playback, holding the current frame while paused.
/// Parameters set while paused still apply once resumed.
inline void set_playing(uint32_t node_id, bool playing) {
    fjell_sprite_anim_set_playing(node_id, playing);
}
} // namespace sprite_anim

} // namespace fjell
