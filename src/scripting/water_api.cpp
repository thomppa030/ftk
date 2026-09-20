#include "fjell/api.h"
#include "fjell/water_api.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/water_surface.hpp"
#include "scene/scene.hpp"
#include "scripting/script_globals.hpp"

namespace {

// The water on a node, or nullptr when it carries none.
const fjell::WaterComponent* water_for(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (scene == nullptr) return nullptr;
    auto* node = scene->find(node_id);
    if (node == nullptr) return nullptr;
    return node->components.get<fjell::WaterComponent>();
}

} // namespace

extern "C" FJELL_SDK_EXPORT int fjell_water_is_water(uint32_t node_id) {
    return water_for(node_id) != nullptr ? 1 : 0;
}

extern "C" FJELL_SDK_EXPORT
float fjell_water_bed_agitation(uint32_t node_id, float depth) {
    const auto* water = water_for(node_id);
    if (water == nullptr) return 0.0F;
    // The effective field, not the authored one: a script asking what this
    // sea is doing means the sea the storm has made of it.
    return fjell::water::bed_agitation(water->effective_water, depth);
}

extern "C" FJELL_SDK_EXPORT float fjell_water_wave_height(uint32_t node_id) {
    const auto* water = water_for(node_id);
    if (water == nullptr) return 0.0F;
    return fjell::water::wave_height(water->effective_water);
}

extern "C" FJELL_SDK_EXPORT
float fjell_water_advance_silt(float load, float agitation, float dt) {
    return fjell::water::advance_silt(load, agitation, dt);
}

extern "C" FJELL_SDK_EXPORT float fjell_water_silt_reference_depth() {
    return fjell::water::SILT_REFERENCE_DEPTH;
}
