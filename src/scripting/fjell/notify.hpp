// notify.hpp — AnimNotify base classes for Fjell game code
// Shipped to game projects alongside fjell_api.h and component.hpp.
#pragma once

namespace fjell {

struct Node3D;

/// Base class for instant animation notifies — one-off events fired from a
/// marker on an animation timeline, so a footstep sound or a hit frame stays
/// in sync with the animation instead of being timed by hand in script.
///
/// Derive, add `FJELL_NOTIFY(T)`, and the notify becomes placeable on any
/// animation timeline in the editor. `[[fj::property]]` fields are editable
/// per placed marker.
///
/// @code
/// struct FootstepNotify : fjell::AnimNotify {
///     FJELL_NOTIFY(FootstepNotify)
///     [[fj::property]] fjell::Ref<fjell::AudioAsset> sound;
///
///     void notify(fjell::Node3D& owner) override {
///         fjell::Audio::play(sound, owner.position());
///     }
/// };
/// @endcode
struct AnimNotify {
    virtual ~AnimNotify() = default;

    /// Called once when playback crosses this marker.
    /// @param owner The node playing the animation.
    /// @note Skipped if the frame is jumped over by a large time step or a
    ///       blend, so do not rely on it for state that must stay balanced.
    virtual void notify(Node3D& owner) = 0;

    /// Type name for serialization. Supplied by `FJELL_NOTIFY`.
    [[nodiscard]] virtual const char* notify_type_name() const = 0;

    /// Whether this is a duration-based notify. False here, true for
    /// `AnimNotifyState`.
    [[nodiscard]] virtual bool is_state() const { return false; }
};

/// Base class for notifies that span a time range rather than firing once —
/// an attack's damage window, a trail effect, an invulnerability period.
/// Placed as a range on the timeline and driven through begin/tick/end.
///
/// @code
/// struct HitboxWindow : fjell::AnimNotifyState {
///     FJELL_NOTIFY(HitboxWindow)
///     void notify_begin(fjell::Node3D& owner) override { enable_hitbox(owner); }
///     void notify_tick(fjell::Node3D& owner, float dt) override {}
///     void notify_end(fjell::Node3D& owner) override { disable_hitbox(owner); }
/// };
/// @endcode
struct AnimNotifyState : public AnimNotify {
    /// Not used by state notifies — the begin/tick/end trio replaces it.
    void notify(Node3D& /*owner*/) override {}

    [[nodiscard]] bool is_state() const override { return true; }

    /// Called once when playback enters the range.
    /// @param owner The node playing the animation.
    virtual void notify_begin(Node3D& owner) = 0;

    /// Called every frame while inside the range.
    /// @param dt Seconds since the previous frame.
    virtual void notify_tick(Node3D& owner, float dt) = 0;

    /// Called once when playback leaves the range. Also fires if the
    /// animation is interrupted mid-range, so it is the right place to undo
    /// whatever `notify_begin()` set up.
    virtual void notify_end(Node3D& owner) = 0;
};

} // namespace fjell
