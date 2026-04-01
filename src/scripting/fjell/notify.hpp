// notify.hpp — AnimNotify base classes for Fjell game code
// Shipped to game projects alongside fjell_api.h and component.hpp.
#pragma once

namespace fjell {

struct Node3D;

// Base class for instant animation notifies.
// Override notify() with self-contained behavior (play sound, spawn VFX, etc.).
struct AnimNotify {
    virtual ~AnimNotify() = default;
    virtual void notify(Node3D& owner) = 0;
    [[nodiscard]] virtual const char* notify_type_name() const = 0;
    [[nodiscard]] virtual bool is_state() const { return false; }
};

// Base class for state span notifies (begin/tick/end).
// Used for effects that persist over a time range on the timeline.
struct AnimNotifyState : public AnimNotify {
    void notify(Node3D& /*owner*/) override {}
    [[nodiscard]] bool is_state() const override { return true; }

    virtual void notify_begin(Node3D& owner) = 0;
    virtual void notify_tick(Node3D& owner, float dt) = 0;
    virtual void notify_end(Node3D& owner) = 0;
};

} // namespace fjell
