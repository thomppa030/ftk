#include "fjell/api.h"
#include "core/log.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_instance.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/scene.hpp"
#include "scripting/script_globals.hpp"

namespace {

// The material state a script may override, on whichever component of the
// node carries one. A mesh and a body of water hold the same pair — a shared
// base material and a lazily cloned instance — so one resolver serves both
// and a script overriding a sea's rain reads exactly like one flashing a hit
// enemy red.
struct MaterialSlot {
    fjell::Material* base{nullptr};
    std::unique_ptr<fjell::MaterialInstance>* instance{nullptr};
};

// Resolve a node_id to its overridable material. Returns an empty slot if the
// node is missing or carries no component that has one.
MaterialSlot slot_for(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return {};
    auto* node = scene->find(node_id);
    if (!node) return {};
    if (auto* mesh = node->components.get<fjell::MeshComponent>()) {
        return {mesh->material, &mesh->instance};
    }
    if (auto* water = node->components.get<fjell::WaterComponent>()) {
        return {water->material, &water->instance};
    }
    return {};
}

// Ensure the slot has a MaterialInstance. Creates one cloning the current
// base material on first call. Returns nullptr if there is no base material
// or required globals are missing.
fjell::MaterialInstance* ensure_mid(const MaterialSlot& slot) {
    if (slot.instance == nullptr) return nullptr;
    if (*slot.instance) return slot.instance->get();
    if (!slot.base) return nullptr;

    auto& svc = fjell::script_services();
    auto* pc = svc.resources ? svc.resources->pipeline_cache() : nullptr;
    if (!svc.resources || !pc) {
        FJELL_GFX_WARN("fjell_mesh_set_param: ResourceManager or PipelineCache not "
                       "available — call set_param after engine init");
        return nullptr;
    }

    *slot.instance = std::make_unique<fjell::MaterialInstance>(
        *slot.base, svc.resources->bindless(), *pc);
    return slot.instance->get();
}

} // namespace

extern "C" FJELL_SDK_EXPORT
void fjell_mesh_set_param_float(uint32_t node_id, const char* name, float value) {
    if (!name) return;
    auto slot = slot_for(node_id);
    auto* mid = ensure_mid(slot);
    if (mid) mid->set_param(name, value);
}

extern "C" FJELL_SDK_EXPORT
void fjell_mesh_set_param_vec2(uint32_t node_id, const char* name, float x, float y) {
    if (!name) return;
    auto slot = slot_for(node_id);
    auto* mid = ensure_mid(slot);
    if (mid) mid->set_param(name, glm::vec2{x, y});
}

extern "C" FJELL_SDK_EXPORT
void fjell_mesh_set_param_vec3(uint32_t node_id, const char* name,
                                float x, float y, float z) {
    if (!name) return;
    auto slot = slot_for(node_id);
    auto* mid = ensure_mid(slot);
    if (mid) mid->set_param(name, glm::vec3{x, y, z});
}

extern "C" FJELL_SDK_EXPORT
void fjell_mesh_set_param_vec4(uint32_t node_id, const char* name,
                                float x, float y, float z, float w) {
    if (!name) return;
    auto slot = slot_for(node_id);
    auto* mid = ensure_mid(slot);
    if (mid) mid->set_param(name, glm::vec4{x, y, z, w});
}

// Drop the MID, returning this node to the shared base material.
extern "C" FJELL_SDK_EXPORT
void fjell_mesh_reset_material(uint32_t node_id) {
    auto slot = slot_for(node_id);
    if (slot.instance != nullptr) slot.instance->reset();
}
