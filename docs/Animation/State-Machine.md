# Animation State Machine

> Stub — fill in.

## Scope
Authoring an `AnimationStateMachine`: states, transitions, conditions, notifies, blend spaces. Driven from script through the component's state machine: `AnimatorComponent::state_machine()` for a skeletal mesh, `SpriteAnimComponent::state_machine` for a sprite.

## Planned sections
- The state machine editor (context tab)
- States — single clip or blend space
- Transitions — duration, blend mode, conditions on params
- Parameters — bool, float, int, trigger
- Notifies (`AnimNotify`)
  - Defining a notify in script (`FJELL_REGISTER_NOTIFY`)
  - Built-in notifies
  - Receiving on the component side
- Blend spaces
  - 1D — `BlendSpace1D` (e.g. speed → idle/walk/run)
  - 2D — `BlendSpace2D` (e.g. velocity vector → 8-way locomotion)
  - Multi-clip blending
- Driving from script:
  ```cpp
  auto& machine = self().get_component<fjell::AnimatorComponent>()->state_machine();
  machine.set_param("speed", glm::length(velocity));
  machine.set_param("jump", 1.0f);   // a trigger is a float too
  ```
- Performance notes — sampling cost, per-component bone budget
