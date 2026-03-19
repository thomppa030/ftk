// fjell_component.hpp — ScriptComponent base class for Fjell game code
// Shipped to game projects alongside fjell_api.h.

#pragma once

#include "fjell_actor.hpp"

namespace fjell {

class ScriptComponent {
public:
    virtual ~ScriptComponent() = default;

    virtual void begin_play() {}
    virtual void tick(float dt) { (void)dt; }
    virtual void end_play() {}

    [[nodiscard]] Actor* actor() { return owner_; }
    [[nodiscard]] const Actor* actor() const { return owner_; }

    [[nodiscard]] World* world() { return owner_ ? owner_->world() : nullptr; }
    [[nodiscard]] const World* world() const { return owner_ ? owner_->world() : nullptr; }

private:
    friend class ScriptManager;
    Actor* owner_{nullptr};
};

} // namespace fjell
