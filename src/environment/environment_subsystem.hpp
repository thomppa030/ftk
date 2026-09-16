#pragma once

#include "core/subsystem.hpp"
#include "environment/environment_state.hpp"

#include <string_view>

namespace fjell {

/// Owns the resolved environment of the scene the engine ticks. Runs every
/// frame, in and out of play, so the editor viewport shows the same lighting
/// the game will: the renderer reads `state()` and never the scene's
/// environment nodes.
class EnvironmentSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "Environment"; }

    void tick(const TickContext& ctx) override;

    [[nodiscard]] const EnvironmentState& state() const { return state_; }

private:
    EnvironmentState state_;
};

} // namespace fjell
