// animator_api.hpp — Skeletal animation API for game scripts
#pragma once

#include "fjell/api.h"
#include "fjell/ref.hpp"

#include <cstdint>

extern "C" {
FJELL_SDK_EXPORT void  fjell_animator_set_param(uint32_t node_id, const char* name, float value);
FJELL_SDK_EXPORT float fjell_animator_get_param(uint32_t node_id, const char* name);
FJELL_SDK_EXPORT const char* fjell_animator_current_state(uint32_t node_id);
}

namespace fjell {

/// Drives skeletal animation on a node with an AnimatorComponent. You set
/// parameters; the animation state machine authored in the editor decides
/// which clip plays and when to blend.
///
/// A node that is gone stops the game (see `Ref`). One with no
/// AnimatorComponent is passed over: a setter does nothing and a getter
/// returns its default.
///
/// @code
/// void tick(float dt) override {
///     fjell::animator::set_param(self(), "speed", velocity_.length());
///     fjell::animator::set_param(self(), "grounded", on_ground_ ? 1.0f : 0.0f);
/// }
/// @endcode
namespace animator {
/// Set a state machine parameter, driving transitions authored in the
/// animation editor.
/// @param node  Node carrying the AnimatorComponent; `self()` for your own.
/// @param name  Parameter name exactly as spelled in the state machine.
///              An unknown name is created rather than rejected, so a
///              typo shows up as an animation that never transitions.
/// @param value Booleans are passed as 0.0 / 1.0.
inline void set_param(const Ref<Node3D>& node, const char* name, float value) {
    detail::require(node);
    fjell_animator_set_param(node.node_id(), name, value);
}

/// Read back a state machine parameter.
/// @return The current value, or 0.0 if the node has no animator or the
///         parameter was never set — the two are indistinguishable.
inline float get_param(const Ref<Node3D>& node, const char* name) {
    detail::require(node);
    return fjell_animator_get_param(node.node_id(), name);
}

/// Name of the state currently playing, as authored in the state machine.
/// @return The state name, or "" if the node has no animator. During a
///         blend this reports the state being blended *from* until the
///         transition completes.
/// @warning Points to engine-owned storage that changes on the next
///          transition. Compare it immediately; copy it if you need to keep it.
inline const char* current_state(const Ref<Node3D>& node) {
    detail::require(node);
    return fjell_animator_current_state(node.node_id());
}
} // namespace animator

} // namespace fjell
