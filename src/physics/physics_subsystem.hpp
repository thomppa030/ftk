#pragma once

#include "core/frame_context.hpp"
#include "core/profiler.hpp"
#include "core/subsystem.hpp"
#include "physics/physics_world.hpp"

#include <memory>
#include <string_view>

namespace fjell {

class PhysicsSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "Physics"; }

    void tick(const TickContext& ctx) override {
        if (!world_) return;
        FJELL_PROFILE_SCOPE_N("physics_step");
        world_->step(ctx.dt, ctx.scene);
    }

    void on_play_start(const TickContext& ctx) override {
        world_ = std::make_unique<PhysicsWorld>();
        world_->create_bodies(ctx.scene);
    }

    void on_play_stop(const TickContext& /*ctx*/) override {
        world_.reset();
    }

    [[nodiscard]] PhysicsWorld* world() { return world_.get(); }

private:
    std::unique_ptr<PhysicsWorld> world_;
};

} // namespace fjell
