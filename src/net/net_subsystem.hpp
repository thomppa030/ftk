// net_subsystem.hpp — Two thin subsystems that drive NetManager in the tick
// loop. They share one NetManager (owned by Engine), the same way Physics owns
// a PhysicsWorld.
//
// Ordering matters (see Plans/done/networking/networking-replication.md "Tick ordering"):
//   NetReceive runs at the FRONT of play_phase_0_ (before scripts), so scripts
//   see up-to-date remote state.
//   NetSend runs in late_always_ (after physics + compute_world_matrices), so it
//   transmits final post-physics transforms, at a fixed network rate.
#pragma once

#include "core/subsystem.hpp"

namespace fjell {

class NetManager;
class ScriptManager;

class NetReceiveSubsystem final : public Subsystem {
public:
    NetReceiveSubsystem(NetManager& net, ScriptManager& scripts)
        : net_(net), scripts_(scripts) {}

    [[nodiscard]] std::string_view name() const override { return "NetReceive"; }

    void tick(const TickContext& ctx) override;

protected:
    void on_play_stop(const TickContext& ctx) override;

private:
    NetManager& net_;
    ScriptManager& scripts_;
};

class NetSendSubsystem final : public Subsystem {
public:
    NetSendSubsystem(NetManager& net, ScriptManager& scripts)
        : net_(net), scripts_(scripts) {}

    [[nodiscard]] std::string_view name() const override { return "NetSend"; }

    void tick(const TickContext& ctx) override;

    // Network send rate (Hz). Set from project config (Step 7); 30 Hz default.
    void set_tick_rate(float hz) { interval_ = (hz > 0.0f) ? (1.0f / hz) : interval_; }

private:
    NetManager& net_;
    ScriptManager& scripts_;
    float accumulator_{0.0f};
    float interval_{1.0f / 30.0f};  // 30 Hz default
};

} // namespace fjell
