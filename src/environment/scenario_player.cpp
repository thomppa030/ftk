#include "environment/environment_scenario.hpp"

#include "environment/environment_subsystem.hpp"
#include "ftk/base/log.hpp"

namespace fjell {

void ScenarioPlayer::tick(float dt, Scene& scene, EnvironmentSubsystem& environment,
                          ResourceManager& resources) {
    if (quit_ || next_ >= scenario_.steps.size()) return;

    time_ += dt;
    while (next_ < scenario_.steps.size() && scenario_.steps[next_].time <= time_) {
        const auto& step = scenario_.steps[next_];
        ++next_;
        if (step.quit) {
            FTK_CORE_INFO("Scenario: quit at {:.1f}s", time_);
            quit_ = true;
            return;
        }
        apply_environment_overrides(step.change, scene, environment, resources);
    }
}

} // namespace fjell
