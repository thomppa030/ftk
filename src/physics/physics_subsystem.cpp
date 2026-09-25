#include "physics/physics_subsystem.hpp"

#include "core/frame_context.hpp"
#include "core/profiler.hpp"
#include "core/thread_pool.hpp"
#include "environment/environment_subsystem.hpp"
#include "physics/buoyancy.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/water_surface.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

namespace fjell {

void PhysicsSubsystem::clear_move_intents(Scene& scene) {
    for (const auto& obj : scene.objects()) {
        if (auto* cc = obj->components.get<CharacterComponent>()) cc->move_intent = glm::vec3{0.0f};
    }
}

void PhysicsSubsystem::keep_frame_intents(Scene& scene) {
    frame_intents_.clear();
    for (const auto& obj : scene.objects()) {
        if (const auto* cc = obj->components.get<CharacterComponent>()) {
            frame_intents_.emplace_back(obj->id, cc->move_intent);
        }
    }
    std::sort(frame_intents_.begin(), frame_intents_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

void PhysicsSubsystem::restore_frame_intents(Scene& scene) {
    for (const auto& obj : scene.objects()) {
        auto* cc = obj->components.get<CharacterComponent>();
        if (cc == nullptr) continue;
        const auto kept = std::lower_bound(
            frame_intents_.begin(), frame_intents_.end(), obj->id,
            [](const auto& entry, uint32_t id) { return entry.first < id; });
        const bool found = kept != frame_intents_.end() && kept->first == obj->id;
        cc->move_intent = found ? kept->second : glm::vec3{0.0f};
    }
}

void PhysicsSubsystem::apply_wind(const FixedTickContext& ctx) {
    if (environment_ == nullptr) return;
    const glm::vec3 wind = environment_->fixed_wind();
    // Below a breeze there is nothing to feel, and walking every body to
    // push it by nothing is not worth the frame.
    if (glm::length(wind) < 0.1f) return;

    for (const auto& obj : ctx.scene.objects()) {
        if (auto* rb = obj->components.get<RigidbodyComponent>();
            rb != nullptr && rb->wind_area > 0.0f && rb->body_type == BodyType::Dynamic) {
            // Drag against still air: the force grows with the square of the
            // wind, which is what makes a gale feel unlike a breeze.
            const float speed = glm::length(wind);
            world_->apply_force(obj->id, glm::normalize(wind) * (speed * speed * rb->wind_area * 0.6f));
        }
        if (auto* cc = obj->components.get<CharacterComponent>(); cc != nullptr && cc->wind_push > 0.0f) {
            // A character is driven by its velocity, not by forces, so the
            // wind moves it directly and the controller resolves it against
            // the ground like any other motion.
            cc->velocity += wind * (cc->wind_push * ctx.dt);
        }
    }
}

namespace {

/// The water a point at `world_xz` is in, or nothing when it is over dry
/// land.
///
/// Infinite water has no edge to be outside of, so it answers for anywhere.
/// A bounded body answers only within its own square, which is what lets a
/// scene hold a sea and a pond without the pond's boats floating at sea
/// level. The first match wins: two overlapping bodies of water is an
/// authoring mistake rather than a case to resolve.
[[nodiscard]] const WaterComponent* water_at(Scene& scene, glm::vec2 world_xz,
                                             float& out_surface_y) {
    for (const auto& obj : scene.objects()) {
        const auto* water = obj->components.get<WaterComponent>();
        if (water == nullptr) continue;
        const auto* spatial = obj->as_spatial();
        if (spatial == nullptr) continue;

        const glm::vec3 centre{scene.current_world_matrix(*spatial)[3]};

        if (water->extent == WaterComponent::Extent::bounded) {
            const float half = water->size * 0.5f;
            if (std::abs(world_xz.x - centre.x) > half ||
                std::abs(world_xz.y - centre.z) > half) {
                continue;
            }
        }
        out_surface_y = centre.y;
        return water;
    }
    return nullptr;
}

} // namespace

void PhysicsSubsystem::apply_buoyancy(const FixedTickContext& ctx) {
    FJELL_PROFILE_SCOPE_N("apply_buoyancy");
    // The sea as it is when this step starts, like every body in the step.
    const auto time = static_cast<float>(ctx.time);

    // Buoyancy is a fight against gravity, so it has to be the same gravity:
    // a scene that has turned it down floats its boats gently rather than by
    // a constant this file made up.
    const float gravity = std::abs(world_->gravity().y);
    if (gravity <= 0.0f) return;

    // Three passes: gather what each floating body is, work out its forces,
    // apply them. The middle pass is the whole cost — every wave summed at
    // every sample point — and each body's is independent of every other's,
    // so it runs across the thread pool. The passes either side stay in scene
    // order on this thread: reading the physics world and writing forces into
    // it in a fixed order is what makes the result the same whichever thread
    // got which body, and the same on every machine.
    //
    // The records are refilled in place rather than rebuilt: this runs every
    // fixed step, and a record keeps the storage its sample points and forces
    // had last step.
    std::size_t used = 0;
    for (const auto& obj : ctx.scene.objects()) {
        auto* buoyancy = obj->components.get<BuoyancyComponent>();
        if (buoyancy == nullptr) continue;

        const auto* rb = obj->components.get<RigidbodyComponent>();
        if (rb == nullptr || rb->body_type != BodyType::Dynamic) continue;

        const auto* spatial = obj->as_spatial();
        if (spatial == nullptr) continue;

        if (used == floating_.size()) floating_.emplace_back();
        Floating& body = floating_[used];
        buoyancy_samples(*buoyancy, obj->components.get<ColliderComponent>(), body.samples);
        if (body.samples.points.empty() || body.samples.volume <= 0.0f) continue;

        body.id = obj->id;
        body.buoyancy = buoyancy;
        body.world = ctx.scene.current_world_matrix(*spatial);
        body.velocity = world_->get_linear_velocity(obj->id);
        body.spin = world_->get_angular_velocity(obj->id);
        body.forces.clear();
        body.any_submerged = false;
        ++used;
    }
    if (used == 0) return;
    const std::span<Floating> floating(floating_.data(), used);

    auto evaluate = [&](Floating& body) {
        const glm::vec3 centre{body.world[3]};
        // Every point carries the same share, so sampling a body more finely
        // resolves its motion without floating it higher.
        const float share = body.samples.volume / static_cast<float>(body.samples.points.size());

        for (const auto& local : body.samples.points) {
            const glm::vec3 point{body.world * glm::vec4(local, 1.0f)};

            float surface_y = 0.0f;
            const WaterComponent* water =
                water_at(ctx.scene, glm::vec2(point.x, point.z), surface_y);
            if (water == nullptr) continue;

            // The wave field is relative to the node holding it, and it is
            // the effective field rather than the authored one: a boat has to
            // float on the sea the storm is blowing. One evaluation gives where
            // the surface is and how it is moving; every wave summed costs the
            // same sine and cosine either way.
            // Every wake but the body's own: rocking in a wake it is making
            // would be feeding its own output back to it.
            const glm::vec2 local_xz(point.x, point.z);
            const water::SurfaceMotion surface =
                water::surface_motion(water->effective_water, local_xz, time, body.id);
            const float wave_y = surface_y + surface.displacement.y;
            const float depth = submersion_depth(point.y, wave_y);
            if (depth <= 0.0f) continue;

            // How this point of the body is moving, which is its own motion
            // plus whatever the body's spin adds out here at the rim. Without
            // the spin term the drag cannot damp a roll, only a heave.
            const glm::vec3 point_velocity =
                body.velocity + glm::cross(body.spin, point - centre);

            // What the water is doing here, so a raft drifts with a swell
            // instead of being dragged through it.
            const glm::vec3 flow =
                body.buoyancy->follow_current ? surface.velocity : glm::vec3{0.0f};

            const BuoyancyForce f = buoyancy_at(depth, share, body.buoyancy->density,
                                                body.buoyancy->drag, point_velocity, flow,
                                                gravity);
            if (!f.submerged) continue;
            body.any_submerged = true;
            body.forces.emplace_back(point, f.force);
        }
    };

    // Handing the bodies to the workers and waiting for them costs about
    // fifteen microseconds whatever they do, so it is only done when there is
    // a good deal more than that to do: the work is one wave summed at one
    // sample point, and the threshold is a few thousand of those, about four
    // dispatches' worth serially. Below it a scene of a few boats on an
    // authored sea runs on this thread. Either way gives the same bits.
    constexpr std::size_t PARALLEL_ABOVE_POINT_WAVES = 2000;
    // An upper estimate: every point on the most waves any water in the scene
    // sums. Which water a point is over is not known until it is evaluated,
    // and a scene rarely has more than one or two.
    std::size_t most_waves = 0;
    for (const auto& obj : ctx.scene.objects()) {
        const auto* water = obj->components.get<WaterComponent>();
        if (water == nullptr) continue;
        const FjWaterData& field = water->effective_water;
        std::size_t waves = field.packed_trains ? field.packed_trains->count : 0;
        if (field.simulated) waves += field.simulated->packed.count;
        for (const water::WakeSource& wake : field.wakes) {
            if (wake.displaces) waves += wake.stamps.size() + 1;
        }
        most_waves = std::max(most_waves, waves);
    }
    std::size_t points = 0;
    for (const Floating& body : floating) points += body.samples.points.size();
    const std::size_t point_waves = points * most_waves;

    // One body per index, so a chunk writes only to its own slots and no two
    // threads touch the same record.
    const auto count = static_cast<uint32_t>(floating.size());
    if (ctx.workers != nullptr && point_waves > PARALLEL_ABOVE_POINT_WAVES) {
        ctx.workers->parallel_for(0, count, [&](uint32_t, uint32_t begin, uint32_t end) {
            for (uint32_t i = begin; i < end; ++i) evaluate(floating[i]);
        });
    } else {
        for (auto& body : floating) evaluate(body);
    }

    for (const Floating& body : floating) {
        for (const auto& [point, force] : body.forces) {
            // At the point rather than at the centre of mass: it is the
            // disagreement between the points that makes a hull pitch and
            // roll instead of merely rising and falling.
            world_->apply_force_at(body.id, force, point);
        }
        // Water resists a body turning as well as moving through it, and a
        // hull that should glide forward may still want to resist being spun.
        if (body.any_submerged && body.buoyancy->angular_drag > 0.0f) {
            world_->set_angular_velocity(
                body.id, body.spin * std::max(1.0f - body.buoyancy->angular_drag * ctx.dt, 0.0f));
        }
    }
}

} // namespace fjell
