#include "fjell/api.h"
#include "core/log.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_instance.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/scene.hpp"
#include "scripting/script_globals.hpp"

namespace {

// Resolve a node_id to its engine-side MeshComponent. Returns nullptr if
// the node is missing or has no mesh.
fjell::MeshComponent* mesh_for(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find(node_id);
    if (!node) return nullptr;
    return node->components.get<fjell::MeshComponent>();
}

// Ensure the mesh has a MaterialInstance. Creates one cloning the current
// base material on first call. Returns nullptr if the mesh has no
// material or required globals are missing.
fjell::MaterialInstance* ensure_mid(fjell::MeshComponent& mesh) {
    if (mesh.instance) return mesh.instance.get();
    if (!mesh.material) return nullptr;

    auto& svc = fjell::script_services();
    auto* pc = svc.resources ? svc.resources->pipeline_cache() : nullptr;
    if (!svc.resources || !pc) {
        FJELL_GFX_WARN("fjell_mesh_set_param: ResourceManager or PipelineCache not "
                       "available — call set_param after engine init");
        return nullptr;
    }

    mesh.instance = std::make_unique<fjell::MaterialInstance>(
        *mesh.material, svc.resources->bindless(), *pc);
    return mesh.instance.get();
}

} // namespace

extern "C" FJELL_SDK_EXPORT
void fjell_mesh_set_param_float(uint32_t node_id, const char* name, float value) {
    if (!name) return;
    auto* mesh = mesh_for(node_id);
    if (!mesh) return;
    auto* mid = ensure_mid(*mesh);
    if (mid) mid->set_param(name, value);
}

extern "C" FJELL_SDK_EXPORT
void fjell_mesh_set_param_vec2(uint32_t node_id, const char* name, float x, float y) {
    if (!name) return;
    auto* mesh = mesh_for(node_id);
    if (!mesh) return;
    auto* mid = ensure_mid(*mesh);
    if (mid) mid->set_param(name, glm::vec2{x, y});
}

extern "C" FJELL_SDK_EXPORT
void fjell_mesh_set_param_vec3(uint32_t node_id, const char* name,
                                float x, float y, float z) {
    if (!name) return;
    auto* mesh = mesh_for(node_id);
    if (!mesh) return;
    auto* mid = ensure_mid(*mesh);
    if (mid) mid->set_param(name, glm::vec3{x, y, z});
}

extern "C" FJELL_SDK_EXPORT
void fjell_mesh_set_param_vec4(uint32_t node_id, const char* name,
                                float x, float y, float z, float w) {
    if (!name) return;
    auto* mesh = mesh_for(node_id);
    if (!mesh) return;
    auto* mid = ensure_mid(*mesh);
    if (mid) mid->set_param(name, glm::vec4{x, y, z, w});
}

// Drop the MID, returning this mesh to the shared base material.
extern "C" FJELL_SDK_EXPORT
void fjell_mesh_reset_material(uint32_t node_id) {
    auto* mesh = mesh_for(node_id);
    if (mesh) mesh->instance.reset();
}
