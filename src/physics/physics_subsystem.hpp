#pragma once

#include "core/frame_context.hpp"
#include "core/profiler.hpp"
#include "core/subsystem.hpp"
#include "physics/buoyancy.hpp"
#include "physics/physics_world.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace fjell {

class EnvironmentSubsystem;

class PhysicsSubsystem : public Subsystem {
public:
    [[nodiscard]] std::string_view name() const override { return "Physics"; }

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

protected:
    void fixed_tick(const FixedTickContext& ctx) override {
        if (!world_) return;
        FJELL_PROFILE_SCOPE_N("physics_step");
        apply_wind(ctx);
        apply_buoyancy(ctx);
        world_->step(ctx.dt, ctx.scene, ctx.frame_steps_left);
    }

private:
    /// Push every body that offers the wind a surface, and lean on every
    /// character that lets it. A force, not an impulse, so a steady wind
    /// holds a body against gravity rather than kicking it once.
    void apply_wind(const FixedTickContext& ctx);

    /// Float every body that has buoyancy on the water it is in, sampling the
    /// wave field at each of its points so a hull pitches and rolls with the
    /// swell rather than riding it flat.
    void apply_buoyancy(const FixedTickContext& ctx);

    /// A body afloat in this step: what it is, where it is, how it moves, and
    /// the forces the water puts on it. Kept from step to step so each one
    /// refills the storage the last one allocated.
    struct Floating {
        uint32_t id{0};
        const BuoyancyComponent* buoyancy{nullptr};
        BuoyancySamples samples;
        glm::mat4 world{1.0f};
        glm::vec3 velocity{0.0f};
        glm::vec3 spin{0.0f};
        // Filled by the parallel pass.
        std::vector<std::pair<glm::vec3, glm::vec3>> forces;   // at point, force
        bool any_submerged{false};
    };
    std::vector<Floating> floating_;

    std::unique_ptr<PhysicsWorld> world_;
    const EnvironmentSubsystem* environment_{nullptr};
};

} // namespace fjell
