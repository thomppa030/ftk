#include "renderer/components/mesh_component.hpp"

#include "renderer/resources/material.hpp"
#include "renderer/resources/material_instance.hpp"

namespace fjell {

MeshComponent::MeshComponent() = default;
MeshComponent::~MeshComponent() = default;
MeshComponent::MeshComponent(MeshComponent&&) noexcept = default;
MeshComponent& MeshComponent::operator=(MeshComponent&&) noexcept = default;

void clone_component(const ComponentStorage& /*from*/, const MeshComponent& src, ComponentStorage& to) {
    auto& dst = to.add<MeshComponent>();
    dst.model_handle = src.model_handle;
    dst.material_handle = src.material_handle;
    dst.model = src.model;
    dst.material = src.material;
    dst.material_name = src.material_name;
    dst.slot_materials = src.slot_materials;
}

uint32_t MeshComponent::resolved_material_id() const {
    if (instance) return instance->material_id();
    return material ? material->material_id() : 0;
}

Material* MeshComponent::material_for_slot(uint32_t slot) const {
    if (slot < slot_materials.size() && slot_materials[slot]) {
        return slot_materials[slot];
    }
    return material;
}

uint32_t MeshComponent::resolved_material_id(uint32_t slot) const {
    // A per-mesh instance overrides the base material, so it also overrides
    // whatever the slot would have resolved to.
    if (instance) return instance->material_id();
    auto* mat = material_for_slot(slot);
    return mat ? mat->material_id() : 0;
}

} // namespace fjell
