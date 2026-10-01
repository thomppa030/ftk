#include "core/subsystem.hpp"

#include "ftk/base/profiler.hpp"

namespace fjell {

void Subsystem::run(const TickContext& ctx) {
    const std::string_view zone = name();
    FJELL_PROFILE_SCOPE_VIEW(zone);
    tick(ctx);
}

void Subsystem::run_fixed(const FixedTickContext& ctx) {
    const std::string_view zone = name();
    FJELL_PROFILE_SCOPE_VIEW(zone);
    fixed_tick(ctx);
}

} // namespace fjell
