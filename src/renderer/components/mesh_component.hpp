#pragma once

#include "renderer/resources/resource_handle.hpp"
#include "scene/node_component.hpp"

#include <memory>
#include <string>

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

    MeshComponent();
    ~MeshComponent();
    MeshComponent(MeshComponent&&) noexcept;
    MeshComponent& operator=(MeshComponent&&) noexcept;
    MeshComponent(const MeshComponent&) = delete;
    MeshComponent& operator=(const MeshComponent&) = delete;

    [[nodiscard]] uint32_t resolved_material_id() const;
};

} // namespace fjell
