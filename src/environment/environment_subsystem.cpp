#include "environment/environment_subsystem.hpp"

#include "core/frame_context.hpp"
#include "environment/environment_resolver.hpp"

namespace fjell {

void EnvironmentSubsystem::tick(const TickContext& ctx) {
    state_ = resolve_environment(ctx.scene);
}

} // namespace fjell
