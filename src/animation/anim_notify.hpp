#pragma once

namespace fjell {

struct Node3D;

// Base class for instant animation notifies. Game code defines notify types
// that override notify() with self-contained behavior (e.g. play sound,
// spawn particle). Placed on animation clip timelines, fired automatically
// when playback crosses their time.
struct AnimNotify {
    virtual ~AnimNotify() = default;

    // Called once when playback crosses this notify's time.
    virtual void notify(Node3D& owner) = 0;

    // Runtime type name (set by FJELL_NOTIFY macro).
    [[nodiscard]] virtual const char* notify_type_name() const = 0;

    // Returns true for AnimNotifyState subclasses (span notifies).
    [[nodiscard]] virtual bool is_state() const { return false; }
};

// Base class for state span notifies (begin/tick/end). Used for effects
// that persist over a time range: weapon trails, invincibility, etc.
// Drawn as colored bars on the timeline instead of diamonds.
struct AnimNotifyState : public AnimNotify {
    void notify(Node3D& /*owner*/) override {} // unused — use begin/tick/end
    [[nodiscard]] bool is_state() const override { return true; }

    virtual void notify_begin(Node3D& owner) = 0;
    virtual void notify_tick(Node3D& owner, float dt) = 0;
    virtual void notify_end(Node3D& owner) = 0;
};

} // namespace fjell
