#include "renderer/components/mesh_component.hpp"

#include "renderer/resources/material.hpp"
#include "renderer/resources/material_instance.hpp"

namespace fjell {

MeshComponent::MeshComponent() = default;
MeshComponent::~MeshComponent() = default;
MeshComponent::MeshComponent(MeshComponent&&) noexcept = default;
MeshComponent& MeshComponent::operator=(MeshComponent&&) noexcept = default;

uint32_t MeshComponent::resolved_material_id() const {
    if (instance) return instance->material_id();
    return material ? material->material_id() : 0;
}

} // namespace fjell
