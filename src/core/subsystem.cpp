#include "core/subsystem.hpp"

#include "core/profiler.hpp"

namespace fjell {

void Subsystem::run(const TickContext& ctx) {
    const std::string_view zone = name();
    FJELL_PROFILE_SCOPE_VIEW(zone);
    tick(ctx);
}

} // namespace fjell
