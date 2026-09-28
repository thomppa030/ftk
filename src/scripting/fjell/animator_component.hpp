// animator_component.hpp — The skeletal animator, as a script reaches it
// Shipped to game projects alongside fjell_api.h.
#pragma once

#include "fjell/api.h"
#include "fjell/animation_state_machine.hpp"

extern "C" {
FJELL_SDK_EXPORT void* fjell_animator_state_machine(const void* animator);
}

namespace fjell {

/// Plays a skeletal mesh's animation from a state machine authored in the
/// editor: parameters go in, and the machine decides which clip or blend
/// space plays and when to cross-fade. The sprite counterpart is
/// `SpriteAnimComponent`, driven the same way through its state machine.
///
/// A script reaches it through a `Ref<AnimatorComponent>` or
/// `get_component<AnimatorComponent>()`, and never holds one by value: it is
/// the engine's component itself, and everything about it goes through its
/// methods.
///
/// @code
/// void begin_play() override { animator_ = self().get_component<fjell::AnimatorComponent>(); }
///
/// void tick(float) override {
///     auto& machine = animator_->state_machine();
///     machine.set_param("speed", glm::length(velocity_));
///     if (machine.current_state_name() == "Land") play_dust();
/// }
/// @endcode
class AnimatorComponent {
public:
    static constexpr const char* type_name = "AnimatorComponent";

    AnimatorComponent() = delete;
    AnimatorComponent(const AnimatorComponent&) = delete;
    AnimatorComponent& operator=(const AnimatorComponent&) = delete;

    /// The state machine choosing what plays: set its parameters, read
    /// `current_state_name()` for the state playing now, bind `on_transition`
    /// to hear it move on. Parameters a blend space reads are smoothed on
    /// their way in, so a blend eases toward a value rather than jumping.
    [[nodiscard]] AnimationStateMachine& state_machine() {
        return *static_cast<AnimationStateMachine*>(fjell_animator_state_machine(this));
    }
    [[nodiscard]] const AnimationStateMachine& state_machine() const {
        return *static_cast<const AnimationStateMachine*>(fjell_animator_state_machine(this));
    }
};

} // namespace fjell
