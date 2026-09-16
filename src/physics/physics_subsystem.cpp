#include "physics/physics_subsystem.hpp"

#include "core/frame_context.hpp"
#include "environment/environment_subsystem.hpp"
#include "physics/physics_types.hpp"
#include "scene/scene.hpp"

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

} // namespace fjell
