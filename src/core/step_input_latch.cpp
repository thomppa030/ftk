#include "core/step_input_latch.hpp"

#include <utility>

namespace fjell {

void StepInputLatch::record(const std::string& action, bool pressed, bool released) {
    if (pressed) pending_pressed_.insert(action);
    if (released) pending_released_.insert(action);
}

void StepInputLatch::begin_step() {
    pressed_ = std::exchange(pending_pressed_, {});
    released_ = std::exchange(pending_released_, {});
}

void StepInputLatch::reset() {
    pending_pressed_.clear();
    pending_released_.clear();
    pressed_.clear();
    released_.clear();
}

bool StepInputLatch::pressed(const std::string& action) const {
    return pressed_.contains(action);
}

bool StepInputLatch::released(const std::string& action) const {
    return released_.contains(action);
}

} // namespace fjell
