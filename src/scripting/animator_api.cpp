#include "fjell/api.h"
#include "animation/animator_component.hpp"

// The animator a script holds is the engine's own component, which a
// Ref<AnimatorComponent> resolved to; its state machine is reached in place.
extern "C" FJELL_SDK_EXPORT void* fjell_animator_state_machine(const void* animator) {
    auto* component = const_cast<fjell::AnimatorComponent*>(static_cast<const fjell::AnimatorComponent*>(animator));
    return &component->state_machine();
}
