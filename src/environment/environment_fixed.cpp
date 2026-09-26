// The environment's side of the simulation: what physics reads from the
// weather and the water, advanced once per fixed step. Everything here reads
// the step's own time and the simulation's placements, never the frame's;
// cmake/FixedTickCheck.cmake holds it to that.
#include "environment/environment_subsystem.hpp"

#include "core/frame_context.hpp"
#include "environment/environment_resolver.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/motion_source_component.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/water_surface.hpp"
#include "scene/scene.hpp"

#include <utility>

namespace fjell {

void EnvironmentSubsystem::fixed_tick(const FixedTickContext& ctx) {
    // The wind the simulation pushes with: the weather the frame resolved,
    // its gust taken at this step's time rather than summed from frames.
    const WeatherParams& weather = state_.weather;
    fixed_wind_ = weather.wind_direction
                * (weather.wind_speed
                   * wind_gust_at(weather.wind_gust_strength, static_cast<float>(ctx.time)));

    // Everything that stirs the water, where the simulation has it this step.
    // A character states its velocity; anything else is measured by how far
    // it went since the last step, which is one step long.
    fixed_sources_.clear();
    std::unordered_map<uint32_t, glm::vec3> positions;
    auto add_source = [&](const Node& node) {
        const auto* spatial = node.as_spatial();
        if (spatial == nullptr) return;
        auto source = motion_source_of(node);
        if (!source) return;

        source->position = glm::vec3(ctx.scene.current_world_matrix(*spatial)[3]);
        if (const auto* character = node.components.get<CharacterComponent>()) {
            source->velocity = character->velocity;
        } else if (auto last = fixed_source_positions_.find(node.id);
                   last != fixed_source_positions_.end() && ctx.dt > 0.0f) {
            source->velocity = (source->position - last->second) / ctx.dt;
        }
        positions.emplace(node.id, source->position);
        fixed_sources_.push_back(*source);
    };
    ctx.scene.each<MotionSourceComponent>(
        [&](const Node& node, const MotionSourceComponent&) { add_source(node); });
    // A character with a source of its own was counted above.
    ctx.scene.each<CharacterComponent>([&](const Node& node, const CharacterComponent&) {
        if (!node.components.has<MotionSourceComponent>()) add_source(node);
    });
    fixed_source_positions_ = std::move(positions);

    advance_water(ctx.scene, fixed_sources_, ctx.dt, static_cast<float>(ctx.time), ctx.workers);
}

void EnvironmentSubsystem::advance_water(Scene& scene, std::span<const MotionSource> sources,
                                         float dt, float time, ThreadPool* workers) {
    // The sustained wind, deliberately without the gust that a sail or a
    // leaf gets. A gust breathes on about a one-second cycle, and a sea that
    // answered it would swing its whole height with it — measured at a gale,
    // between 0.93x and 2.27x every second, the entire ocean heaving. Real
    // water does not work that way: wind takes minutes to hours to raise a
    // sea, and what a gust does meanwhile is ruffle the surface, which is the
    // material's business and not the wave field's.
    const glm::vec3 wind = state_.weather.wind_direction * state_.weather.wind_speed;
    const glm::vec2 blowing{wind.x, wind.z};

    // Water has mass: the field follows the wind rather than tracking it, so
    // a front raises a sea over its passage instead of at its arrival. The
    // curve and its rates live beside the evaluator, where they are tested.
    if (!sea_wind_.has_value()) {
        // First tick: start at what is already blowing, so a scene opens with
        // the sea its weather implies instead of building one up from calm.
        sea_wind_ = blowing;
    }
    sea_wind_ = water::advance_sea_wind(*sea_wind_, blowing, dt);

    const glm::vec2 wind_xz = *sea_wind_;

    scene.each<WaterComponent>([&](Node& node, WaterComponent& water_component) {
        auto* water = &water_component;
        water->effective_water = water->derivation.derive(water->water, wind_xz, workers);

        // What has moved through it, on top of the sea the wind made. The
        // trails are laid against that sea, since whether a wader's feet are
        // in the water depends on where the waves have put it.
        const auto* spatial = node.as_spatial();
        if (spatial == nullptr) return;
        const glm::vec3 centre{scene.current_world_matrix(*spatial)[3]};
        water::WakePlacement placement;
        placement.surface_y = centre.y;
        placement.vertex_spacing = water->vertex_spacing();
        if (water->extent == WaterComponent::Extent::bounded) {
            const glm::vec2 half(water->size * 0.5f);
            placement.square = std::make_pair(glm::vec2(centre.x, centre.z) - half,
                                              glm::vec2(centre.x, centre.z) + half);
        }
        water->wakes.advance(sources, water->effective_water, placement, time);
        water->effective_water.wakes = water->wakes.sources();
    });
}

} // namespace fjell
