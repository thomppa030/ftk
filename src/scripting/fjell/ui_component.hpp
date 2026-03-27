// ui_component.hpp — UiComponent base class for widget controllers
#pragma once

#include "fjell/world.hpp"

#include <cstdint>

namespace fjell {

class UiSubsystem;

/// Base class for widget controllers. Bridges game state to a widget's
/// scoped UiContext via [[fj::bind]] fields and [[fj::action]] methods.
///
/// Unlike ScriptComponent, UiComponent is not attached to a scene node.
/// It is tied to a widget tag via FJELL_UI_COMPONENT(Name, "tag").
class UiComponent {
public:
    virtual ~UiComponent() = default;

    virtual void begin_play() {}
    virtual void tick(float dt) { (void)dt; }
    virtual void end_play() {}

    [[nodiscard]] float delta_time() const { return world_->delta_time; }
    [[nodiscard]] float elapsed_time() const { return world_->elapsed_time; }
    [[nodiscard]] const Input& input() const { return world_->input(); }
    [[nodiscard]] World* world() { return world_; }
    [[nodiscard]] const World* world() const { return world_; }

private:
    friend class UiSubsystem;
    World* world_{nullptr};
    uint32_t id_{0};
};

} // namespace fjell
