#include "renderer/components/water_component.hpp"

#include "renderer/resources/material.hpp"
#include "renderer/resources/material_instance.hpp"

namespace fjell {

// Out of line: `instance` holds a forward-declared MaterialInstance, so the
// destructor and moves need its definition in scope.
WaterComponent::WaterComponent() = default;
WaterComponent::~WaterComponent() = default;
WaterComponent::WaterComponent(WaterComponent&&) noexcept = default;
WaterComponent& WaterComponent::operator=(WaterComponent&&) noexcept = default;

std::uint32_t WaterComponent::resolved_material_id() const {
    if (instance) return instance->material_id();
    return material != nullptr ? material->material_id() : 0;
}

void clone_component(const ComponentStorage& /*from*/, const WaterComponent& src,
                     ComponentStorage& to) {
    auto& dst = to.add<WaterComponent>();
    dst.extent = src.extent;
    dst.size = src.size;
    dst.patches_per_side = src.patches_per_side;
    dst.water_path = src.water_path;
    dst.water = src.water;
    dst.water_loaded = src.water_loaded;
    dst.effective_water = src.effective_water;
    dst.material_handle = src.material_handle;
    dst.material = src.material;
    dst.medium_material_handle = src.medium_material_handle;
    dst.medium_material = src.medium_material;
    dst.medium_material_path = src.medium_material_path;
    dst.material_path = src.material_path;
}

} // namespace fjell
