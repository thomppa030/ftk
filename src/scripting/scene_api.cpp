#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "scripting/script_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/preset_asset.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scene/removal_epoch.hpp"
#include "scene/component_registry.hpp"
#include "physics/physics_world.hpp"

#include <cstring>

namespace {

// A node as a script holds it is the engine's own: what a Ref<Node3D>
// resolved to, or a script's own node. Only 3D nodes are handed out.
fjell::Node3D& node_at(void* node) { return *static_cast<fjell::Node3D*>(node); }
const fjell::Node3D& node_at(const void* node) { return *static_cast<const fjell::Node3D*>(node); }

} // namespace

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_find_node(const char* name) {
    auto* scene = fjell::script_services().scene;
    if (!scene || !name) return 0;
    const fjell::Node* node = scene->find_by_name(name);
    return node != nullptr ? node->id : 0;
}

// The type id scripts know by `type_name`. The library's own script types
// come first: a script type may share a name only with an engine component
// the library has no mirror of (the load refuses the others), so to that
// library the name means its script. The answer holds for as long as the
// library is loaded, and the SDK asks once per type.
extern "C" FJELL_SDK_EXPORT uint32_t fjell_component_type_id(const char* type_name) {
    if (!type_name) return FJELL_NO_COMPONENT_TYPE;
    if (auto* manager = fjell::script_services().manager) {
        if (const auto* type = manager->find_script_type(type_name)) return type->component.id;
    }
    if (auto id = fjell::ComponentRegistry::instance().find_id_by_script_name(type_name)) return *id;
    return FJELL_NO_COMPONENT_TYPE;
}

// What a Ref finds: the component of `type_id` on the node, or for a
// Ref<Node3D> the node itself when it is a 3D node.
extern "C" FJELL_SDK_EXPORT void* fjell_ref_resolve(uint32_t node_id, uint32_t generation,
                                                   uint32_t type_id) {
    auto* scene = fjell::script_services().scene;
    if (scene == nullptr) return nullptr;
    if (type_id == FJELL_NODE_TYPE) {
        fjell::Node* node = scene->resolve_node(node_id, generation);
        return node != nullptr ? static_cast<void*>(node->as_spatial()) : nullptr;
    }
    return scene->resolve(node_id, generation, type_id);
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_generation(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    return scene != nullptr ? scene->generation_of(node_id) : 0;
}

extern "C" FJELL_SDK_EXPORT const std::atomic<uint64_t>* fjell_removal_epoch() {
    return &fjell::removal_epoch_counter();
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_find(uint32_t type_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return 0;
    const fjell::Node* node = scene->first_holder(type_id);
    return node ? node->id : 0;
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_empty(const char* name,
                                            float x, float y, float z) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return 0;
    fjell::Transform t;
    t.position = {x, y, z};
    return scene->add_empty(t, name ? name : "Spawned");
}

// `parent_id` 0 spawns at the scene root.
extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_preset(uint32_t handle_id,
                                                              float x, float y, float z,
                                                              uint32_t parent_id) {
    auto& s = fjell::script_services();
    if (!s.scene || !s.resources) return 0;

    auto* asset = s.resources->get(fjell::PresetAssetHandle{handle_id});
    if (!asset) return 0;

    fjell::Transform offset;
    offset.position = {x, y, z};
    return asset->preset.instantiate(*s.scene, parent_id != 0 ? static_cast<int32_t>(parent_id) : -1, offset);
}

// ── A node's own calls, on the node a script holds ─────────────────────

extern "C" FJELL_SDK_EXPORT void fjell_node_identity(const void* node, uint32_t* node_id,
                                                    uint32_t* generation) {
    *node_id = node_at(node).id;
    *generation = node_at(node).generation;
}

extern "C" FJELL_SDK_EXPORT float* fjell_node_position(void* node) {
    return &node_at(node).transform.position.x;
}

extern "C" FJELL_SDK_EXPORT float* fjell_node_rotation(void* node) {
    return &node_at(node).transform.rotation.x;
}

extern "C" FJELL_SDK_EXPORT float* fjell_node_scale(void* node) {
    return &node_at(node).transform.scale.x;
}

extern "C" FJELL_SDK_EXPORT void fjell_node_render_transform(const void* node, float* position,
                                                            float* rotation, float* scale) {
    auto* scene = fjell::script_services().scene;
    const fjell::Transform drawn = scene != nullptr
                                       ? fjell::decompose_matrix(scene->interpolated_world_matrix(node_at(node)))
                                       : node_at(node).transform;
    std::memcpy(position, &drawn.position, sizeof(drawn.position));
    std::memcpy(rotation, &drawn.rotation, sizeof(drawn.rotation));
    std::memcpy(scale, &drawn.scale, sizeof(drawn.scale));
}

extern "C" FJELL_SDK_EXPORT void fjell_node_teleport(void* node, const float* position,
                                                    const float* rotation) {
    auto& services = fjell::script_services();
    if (!services.scene) return;
    glm::vec3 world_position;
    glm::quat world_rotation;
    std::memcpy(&world_position, position, sizeof(world_position));
    std::memcpy(&world_rotation, rotation, sizeof(world_rotation));
    services.scene->teleport(node_at(node), world_position, world_rotation);
    // A body's physics would otherwise put the node back where it was.
    if (services.physics) services.physics->teleport(node_at(node).id, world_position, world_rotation);
}

extern "C" FJELL_SDK_EXPORT void fjell_node_add_component(void* node, uint32_t type_id) {
    auto& services = fjell::script_services();
    if (!services.scene) return;
    fjell::Node3D& target = node_at(node);

    if (const auto* type = services.manager ? services.manager->find_script_type_by_id(type_id)
                                            : nullptr) {
        services.manager->add_script(target, type->name);
        services.scene->mark_structure_changed();
        return;
    }

    const auto* meta = fjell::ComponentRegistry::instance().find(type_id);
    if (meta && meta->script_add) meta->script_add(target);
}

// A direct child by name. Names repeat across instances (each spawned player
// has its own "Camera"); the parent says which one.
extern "C" FJELL_SDK_EXPORT uint32_t fjell_node_find_child(const void* node, const char* name) {
    auto* scene = fjell::script_services().scene;
    if (!scene || !name) return 0;
    const fjell::Node* child = scene->find_child(node_at(node).id, name);
    return child != nullptr ? child->id : 0;
}

extern "C" FJELL_SDK_EXPORT void fjell_node_destroy(const void* node) {
    auto* scene = fjell::script_services().scene;
    if (scene) scene->queue_destroy(node_at(node).id);
}
