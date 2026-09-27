// sprite_anim_component.hpp — Sprite animation component for game scripts.
// Layout-matching copy of renderer/sprite_anim_component.hpp, shipped as part of the SDK.
#pragma once

#include "fjell/animation_state_machine.hpp"
#include "fjell/sprite_sheet.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace fjell {

/// Drives a SpriteComponent's frames from a sprite animation set. Needs a
/// SpriteComponent on the same node.
///
/// Prefer the `sprite_anim` functions over touching these fields — it resolves the
/// node and handles a missing component safely.
struct SpriteAnimComponent {
    static constexpr const char* type_name = "SpriteAnimComponent";

    /// Path to the animation set asset defining the sheets and state machine.
    std::string animset_path;

    /// Playback rate multiplier applied on top of each sheet's `fps`.
    float playback_speed{1.0f};

    /// @{
    /// @deprecated Pre-state-machine animation list, kept so older projects
    ///             keep loading. Use `animset_path` instead.
    std::unordered_map<std::string, std::string> animations;
    std::string default_animation;
    /// @}

    /// The state machine choosing which sheet plays. Drive it through
    /// `sprite_anim::set_param()`.
    AnimationStateMachine state_machine;

    /// Loaded sheets, keyed by animation name. Populated when the set loads.
    std::unordered_map<std::string, SpriteSheet> sheets;

    /// Frame index within the current sheet.
    uint32_t current_frame{0};

    /// Seconds accumulated toward the next frame. Engine-owned.
    float frame_timer{0.0f};

    /// Whether playback is advancing — see `sprite_anim::set_playing()`.
    bool playing{true};

    /// Whether a non-looping animation reached its last frame. Always false
    /// while looping. See `sprite_anim::is_finished()`.
    bool finished{false};

    /// Whether the animation set has finished loading. Sheets and state
    /// machine are empty until then.
    bool loaded{false};
};

} // namespace fjell
