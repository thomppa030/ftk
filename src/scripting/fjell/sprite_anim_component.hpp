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
/// Scripts drive it directly: parameters on `state_machine` choose what
/// plays, `playing` pauses it, `finished` says a one-shot is done. To face
/// left or right, set the SpriteComponent's `flip_h`.
///
/// @code
/// [[fj::property]] fjell::Ref<fjell::SpriteAnimComponent> anim;
///
/// void tick(float) override {
///     anim->state_machine.set_param("speed", std::abs(velocity_.x));
///     if (anim->finished) anim->state_machine.set_param("attacking", 0.0f);
/// }
/// @endcode
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

    /// The state machine choosing which sheet plays: set its parameters, and
    /// read `current_state_name()` for what plays now.
    AnimationStateMachine state_machine;

    /// Loaded sheets, keyed by animation name. Populated when the set loads.
    std::unordered_map<std::string, SpriteSheet> sheets;

    /// Frame index within the current sheet.
    uint32_t current_frame{0};

    /// Seconds accumulated toward the next frame. Engine-owned.
    float frame_timer{0.0f};

    /// Whether playback is advancing. Clear it to hold the current frame;
    /// parameters set meanwhile apply once it plays again.
    bool playing{true};

    /// Whether a non-looping animation reached its last frame. Always false
    /// while looping, and true until a different state begins.
    bool finished{false};

    /// Whether the animation set has finished loading. Sheets and state
    /// machine are empty until then.
    bool loaded{false};
};

} // namespace fjell
