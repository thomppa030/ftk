#pragma once

#include "core/frame_context.hpp"
#include "core/subsystem.hpp"
#include "ftk/base/profiler.hpp"
#include "physics/buoyancy.hpp"
#include "physics/physics_world.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace fjell {

struct WaterComponent;

class EnvironmentSubsystem;
class Scene;

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

    /// Clear every character's move intent. A frame's intent is that frame's:
    /// cleared before input fires, a held stick sets it again and a released
    /// one leaves it zero, without a script having to reset it.
    void clear_move_intents(Scene& scene);

    /// Keep every character's move intent as the frame left it — from input
    /// handlers and tick() — before the frame's steps. Each step starts from
    /// it: the frame's intent steers every step of the frame, and one a
    /// script sets in fixed_tick() steers its own step and no other, wherever
    /// the frame boundaries fall.
    void keep_frame_intents(Scene& scene);

protected:
    void fixed_tick(const FixedTickContext& ctx) override {
        if (!world_) return;
        FTK_PROFILE_SCOPE_N("physics_step");
        apply_wind(ctx);
        apply_buoyancy(ctx);
        world_->step(ctx.dt, ctx.scene, ctx.frame_steps_left);
        restore_frame_intents(ctx.scene);
    }

private:
    /// Push every body that offers the wind a surface, and lean on every
    /// character that lets it. A force, not an impulse, so a steady wind
    /// holds a body against gravity rather than kicking it once.
    void apply_wind(const FixedTickContext& ctx);

    /// Once a step has steered by it, put back the intent the frame left, so
    /// the next step starts from it. A character spawned during the frame had
    /// none and goes back to zero.
    void restore_frame_intents(Scene& scene);

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

    /// A body of water as a step finds it, gathered once on the step's own
    /// thread: the buoyancy pass reads it from the workers, which must not
    /// walk the scene.
    struct WaterBody {
        const WaterComponent* water;
        glm::vec3 centre;
    };
    std::vector<WaterBody> waters_;
    [[nodiscard]] static const WaterComponent* water_at(std::span<const WaterBody> waters,
                                                        glm::vec2 world_xz, float& out_surface_y);

    // Each character's intent as the frame left it, by node id, sorted.
    std::vector<std::pair<uint32_t, glm::vec3>> frame_intents_;

    std::unique_ptr<PhysicsWorld> world_;
    const EnvironmentSubsystem* environment_{nullptr};
};

} // namespace fjell
