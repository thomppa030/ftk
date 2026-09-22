#include "physics/physics_subsystem.hpp"

#include "core/clock.hpp"
#include "core/frame_context.hpp"
#include "core/profiler.hpp"
#include "environment/environment_subsystem.hpp"
#include "physics/buoyancy.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/water_surface.hpp"
#include "scene/scene.hpp"

#include <cmath>

namespace fjell {

void PhysicsSubsystem::apply_wind(const TickContext& ctx) {
    if (environment_ == nullptr) return;
    const auto& weather = environment_->state().weather;
    const glm::vec3 wind = weather.wind_direction * (weather.wind_speed * weather.wind_gust);
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

void PhysicsSubsystem::apply_buoyancy(const TickContext& ctx) {
    FJELL_PROFILE_SCOPE_N("apply_buoyancy");
    // The same clock the renderer advances its wave field with, so a hull
    // floats on the wave the player is looking at rather than one a frame
    // either side of it.
    const float time = ctx.clock.game_elapsed();

    // Buoyancy is a fight against gravity, so it has to be the same gravity:
    // a scene that has turned it down floats its boats gently rather than by
    // a constant this file made up.
    const float gravity = std::abs(world_->gravity().y);
    if (gravity <= 0.0f) return;

    for (const auto& obj : ctx.scene.objects()) {
        auto* buoyancy = obj->components.get<BuoyancyComponent>();
        if (buoyancy == nullptr) continue;

        const auto* rb = obj->components.get<RigidbodyComponent>();
        if (rb == nullptr || rb->body_type != BodyType::Dynamic) continue;

        const BuoyancySamples samples =
            buoyancy_samples(*buoyancy, obj->components.get<ColliderComponent>());
        if (samples.points.empty() || samples.volume <= 0.0f) continue;

        const auto* spatial = obj->as_spatial();
        if (spatial == nullptr) continue;

        const glm::mat4 world = ctx.scene.current_world_matrix(*spatial);
        const glm::vec3 body_velocity = world_->get_linear_velocity(obj->id);
        const glm::vec3 body_spin = world_->get_angular_velocity(obj->id);
        const glm::vec3 centre{world[3]};

        // Every point carries the same share, so sampling a body more finely
        // resolves its motion without floating it higher.
        const float share = samples.volume / static_cast<float>(samples.points.size());

        bool any_submerged = false;
        for (const auto& local : samples.points) {
            const glm::vec3 point{world * glm::vec4(local, 1.0f)};

            float surface_y = 0.0f;
            const WaterComponent* water =
                water_at(ctx.scene, glm::vec2(point.x, point.z), surface_y);
            if (water == nullptr) continue;

            // The wave field is relative to the node holding it, and it is
            // the effective field rather than the authored one: a boat has to
            // float on the sea the storm is blowing. One evaluation gives where
            // the surface is and how it is moving; every wave summed costs the
            // same sine and cosine either way.
            const glm::vec2 local_xz(point.x, point.z);
            const water::SurfaceMotion surface =
                water::surface_motion(water->effective_water, local_xz, time);
            const float wave_y = surface_y + surface.displacement.y;
            const float depth = submersion_depth(point.y, wave_y);
            if (depth <= 0.0f) continue;

            // How this point of the body is moving, which is its own motion
            // plus whatever the body's spin adds out here at the rim. Without
            // the spin term the drag cannot damp a roll, only a heave.
            const glm::vec3 point_velocity =
                body_velocity + glm::cross(body_spin, point - centre);

            // What the water is doing here, so a raft drifts with a swell
            // instead of being dragged through it.
            const glm::vec3 flow = buoyancy->follow_current ? surface.velocity : glm::vec3{0.0f};

            const BuoyancyForce f = buoyancy_at(depth, share, buoyancy->density,
                                                buoyancy->drag, point_velocity, flow,
                                                gravity);
            if (!f.submerged) continue;
            any_submerged = true;

            // At the point rather than at the centre of mass: it is the
            // disagreement between the points that makes a hull pitch and
            // roll instead of merely rising and falling.
            world_->apply_force_at(obj->id, f.force, point);
        }

        // Water resists a body turning as well as moving through it, and a
        // hull that should glide forward may still want to resist being spun.
        if (any_submerged && buoyancy->angular_drag > 0.0f) {
            world_->set_angular_velocity(
                obj->id, body_spin * std::max(1.0f - buoyancy->angular_drag * ctx.dt, 0.0f));
        }
    }
}

} // namespace fjell
