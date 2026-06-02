#include "net/net_subsystem.hpp"

#include "net/net_manager.hpp"
#include "core/frame_context.hpp"

namespace fjell {

void NetReceiveSubsystem::tick(const TickContext& ctx) {
    net_.receive_and_apply(ctx.scene);
}

void NetReceiveSubsystem::on_play_stop(const TickContext& /*ctx*/) {
    // Leaving play disconnects any active session so a fresh play start is clean.
    net_.shutdown();
}

void NetSendSubsystem::tick(const TickContext& ctx) {
    accumulator_ += ctx.dt;
    if (accumulator_ < interval_) return;
    accumulator_ -= interval_;
    net_.collect_and_send(ctx.scene);
}

} // namespace fjell
