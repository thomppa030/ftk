#include "ui/inspectors/inspector_registry.hpp"

#include "animation/animator_component.hpp"
#include "audio/audio_component.hpp"
#include "core/input_component.hpp"
#include "core/log.hpp"
#include "physics/buoyancy.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/arrow_component.hpp"
#include "renderer/components/instanced_mesh_component.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/motion_source_component.hpp"
#include "renderer/components/particle_emitter.hpp"
#include "renderer/components/skeletal_mesh_component.hpp"
#include "renderer/components/spline_component.hpp"
#include "renderer/components/spline_mesh_component.hpp"
#include "renderer/components/sprite_anim_component.hpp"
#include "renderer/components/sprite_desc.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/components/tilemap_component.hpp"
#include "renderer/components/water_component.hpp"
#include "scene/component_registry.hpp"
#include "ui/inspectors/animation_inspectors.hpp"
#include "ui/inspectors/environment_inspectors.hpp"
#include "ui/inspectors/gameplay_inspectors.hpp"
#include "ui/inspectors/physics_inspectors.hpp"
#include "ui/inspectors/rendering_inspectors.hpp"
#include "ui/inspectors/spline_inspectors.hpp"
#include "ui/game_ui/ui_widget_component.hpp"

namespace fjell {

const ComponentInspector* InspectorRegistry::find(ComponentTypeId id) const {
    for (const auto& e : entries_) {
        if (e.id == id) return &e.inspector;
    }
    return nullptr;
}

namespace {

// One line per component. A component without an entry has no category
// and no inspector, which is reported when the list is first built.
InspectorRegistry build() {
    using C = theme::Category;
    InspectorRegistry r;

    r.add<MeshComponent>({.category = C::Rendering, .draw = draw_mesh});
    r.add<SkeletalMeshComponent>({.category = C::Rendering, .draw = draw_skeletal_mesh});
    r.add<InstancedMeshComponent>({.category = C::Rendering, .draw = draw_instanced_mesh});
    r.add<SpriteComponent>({.category = C::Rendering, .draw = draw_sprite});
    r.add<TilemapComponent>({.category = C::Rendering, .draw = draw_tilemap});
    r.add<SplineMeshComponent>({.category = C::Rendering, .draw = draw_spline_mesh});

    r.add<TerrainComponent>({.category = C::Environment, .draw = draw_terrain});
    r.add<WaterComponent>({.category = C::Environment, .draw = draw_water});
    r.add<MotionSourceComponent>({.category = C::Environment, .draw = draw_motion_source});

    r.add<RigidbodyComponent>({.category = C::Physics, .draw = draw_rigidbody});
    r.add<ColliderComponent>({.category = C::Physics, .draw = draw_collider});
    r.add<CharacterComponent>({.category = C::Physics, .draw = draw_character});
    r.add<BuoyancyComponent>({.category = C::Physics, .draw = draw_buoyancy});

    r.add<AnimatorComponent>({.category = C::Animation, .draw = draw_animator});
    r.add<SpriteAnimComponent>({.category = C::Animation, .draw = draw_sprite_animation});

    r.add<AudioSourceComponent>({.category = C::Audio, .draw = draw_audio_source});
    r.add<EmitterComponent>({.category = C::Vfx, .draw = draw_particle_emitter});
    r.add<UiWidgetComponent>({.category = C::Ui, .draw = draw_ui_widget});
    r.add<InputComponent>({.category = C::Logic, .draw = draw_input});

    r.add<SplineComponent>({.category = C::Structure, .draw = draw_spline});
    r.add<ArrowComponent>({.category = C::Structure, .draw = draw_arrow});

    ComponentRegistry::instance().for_each([&](ComponentTypeId id, const ComponentMeta& meta) {
        const auto* inspector = r.find(id);
        if (inspector == nullptr || inspector->draw == nullptr) {
            FJELL_CORE_ERROR("Component '{}' has no inspector in ui/inspectors/inspector_registry.cpp",
                             meta.display_name);
        }
    });
    return r;
}

} // namespace

const InspectorRegistry& component_inspectors() {
    static const InspectorRegistry registry = build();
    return registry;
}

} // namespace fjell
