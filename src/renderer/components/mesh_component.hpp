#pragma once

#include "renderer/resources/resource_handle.hpp"
#include "scene/component_storage.hpp"
#include "scene/node_component.hpp"

#include <memory>
#include <string>
#include <vector>

namespace fjell {

class Material;
class MaterialInstance;
class Model;

struct MeshComponent : NodeComponent {
    ModelHandle model_handle;
    MaterialHandle material_handle;
    Model* model{nullptr};
    Material* material{nullptr};
    std::string material_name;

    // Per-mesh override of the base material. Created on first set_param;
    // drops back to `material` when reset. The renderer reads
    // resolved_material_id() each frame to pick the SSBO row.
    std::unique_ptr<MaterialInstance> instance;

    // Material per slot, from the mesh's authored slot assignments. Empty
    // means the whole model draws with `material`. A slot that resolved to
    // nothing holds a null entry and falls back the same way.
    std::vector<Material*> slot_materials;

    MeshComponent();
    ~MeshComponent();
    MeshComponent(MeshComponent&&) noexcept;
    MeshComponent& operator=(MeshComponent&&) noexcept;
    MeshComponent(const MeshComponent&) = delete;
    MeshComponent& operator=(const MeshComponent&) = delete;

    [[nodiscard]] uint32_t resolved_material_id() const;

    // Material for one of the model's submeshes. Falls back to the object's
    // material where the mesh has no slot for it or the slot is unassigned.
    [[nodiscard]] Material* material_for_slot(uint32_t slot) const;
    [[nodiscard]] uint32_t resolved_material_id(uint32_t slot) const;
};

// A duplicate renders the same model with the same materials but starts
// without the per-mesh MaterialInstance: that is runtime override state that
// belongs to the one mesh it was set on.
void clone_component(const ComponentStorage& from, const MeshComponent& src, ComponentStorage& to);

} // namespace fjell
