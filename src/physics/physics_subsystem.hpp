#pragma once

#include "core/frame_context.hpp"
#include "core/profiler.hpp"
#include "core/subsystem.hpp"
#include "physics/physics_world.hpp"

#include <memory>
#include <string_view>

namespace fjell {

class EnvironmentSubsystem;

class PhysicsSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "Physics"; }

    void tick(const TickContext& ctx) override {
        if (!world_) return;
        FJELL_PROFILE_SCOPE_N("physics_step");
        apply_wind(ctx);
        apply_buoyancy(ctx);
        world_->step(ctx.dt, ctx.scene);
    }

    /// Where the weather's wind comes from. Optional; without it nothing
    /// blows.
    void set_environment(const EnvironmentSubsystem* environment) { environment_ = environment; }

    void on_play_start(const TickContext& ctx) override {
        world_ = std::make_unique<PhysicsWorld>();
        world_->create_bodies(ctx.scene);
    }

    void on_play_stop(const TickContext& /*ctx*/) override {
        world_.reset();
    }

    [[nodiscard]] PhysicsWorld* world() { return world_.get(); }

private:
    /// Push every body that offers the wind a surface, and lean on every
    /// character that lets it. A force, not an impulse, so a steady wind
    /// holds a body against gravity rather than kicking it once.
    void apply_wind(const TickContext& ctx);

    /// Float every body that has buoyancy on the water it is in, sampling the
    /// wave field at each of its points so a hull pitches and rolls with the
    /// swell rather than riding it flat.
    void apply_buoyancy(const TickContext& ctx);

    std::unique_ptr<PhysicsWorld> world_;
    const EnvironmentSubsystem* environment_{nullptr};
};

} // namespace fjell
