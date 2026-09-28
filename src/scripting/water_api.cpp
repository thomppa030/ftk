#include "fjell/api.h"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/water_surface.hpp"

namespace {

// The water a script holds is the engine's own component, which a
// Ref<WaterComponent> resolved to.
const fjell::WaterComponent& water_at(const void* water) { return *static_cast<const fjell::WaterComponent*>(water); }

} // namespace

// The effective field, not the authored one: a script asking what this sea
// is doing means the sea the storm has made of it.
extern "C" FJELL_SDK_EXPORT float fjell_water_bed_agitation(const void* water, float depth) {
    return fjell::water::bed_agitation(water_at(water).effective_water, depth);
}

extern "C" FJELL_SDK_EXPORT float fjell_water_wave_height(const void* water) {
    return fjell::water::wave_height(water_at(water).effective_water);
}

extern "C" FJELL_SDK_EXPORT float fjell_water_advance_silt(float load, float agitation, float dt) {
    return fjell::water::advance_silt(load, agitation, dt);
}

extern "C" FJELL_SDK_EXPORT float fjell_water_silt_reference_depth() {
    return fjell::water::SILT_REFERENCE_DEPTH;
}
