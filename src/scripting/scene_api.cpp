#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "scripting/script_manager.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "scene/preset_asset.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "scene/component_registry.hpp"
#include "physics/physics_world.hpp"

#include <cstring>

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_find_node(const char* name) {
    auto* scene = fjell::script_services().scene;
    if (!scene || !name) return 0;
    for (const auto& obj : scene->objects()) {
        if (obj->name == name) return obj->id;
    }
    return 0;
}

// Find a direct child of parent_id by name. Disambiguates duplicate names
// across instances (e.g. each spawned player has its own "Camera" child).
extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_find_child(uint32_t parent_id,
                                                            const char* name) {
    auto* scene = fjell::script_services().scene;
    if (!scene || !name) return 0;
    for (const auto& obj : scene->objects()) {
        if (obj->parent_id == static_cast<int32_t>(parent_id) && obj->name == name) {
            return obj->id;
        }
    }
    return 0;
}

extern "C" FJELL_SDK_EXPORT void fjell_scene_destroy(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (scene) scene->queue_destroy(node_id);
}

extern "C" FJELL_SDK_EXPORT float* fjell_scene_get_position(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find_spatial(node_id);
    return node ? &node->transform.position.x : nullptr;
}

extern "C" FJELL_SDK_EXPORT float* fjell_scene_get_scale(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find_spatial(node_id);
    return node ? &node->transform.scale.x : nullptr;
}

extern "C" FJELL_SDK_EXPORT float* fjell_scene_get_rotation(uint32_t node_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find_spatial(node_id);
    return node ? &node->transform.rotation.x : nullptr;
}

extern "C" FJELL_SDK_EXPORT bool fjell_scene_get_render_transform(uint32_t node_id,
                                                                  float* position,
                                                                  float* rotation,
                                                                  float* scale) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return false;
    const auto* node = scene->find_spatial(node_id);
    if (!node) return false;
    const fjell::Transform drawn =
        fjell::decompose_matrix(scene->interpolated_world_matrix(*node));
    std::memcpy(position, &drawn.position, sizeof(drawn.position));
    std::memcpy(rotation, &drawn.rotation, sizeof(drawn.rotation));
    std::memcpy(scale, &drawn.scale, sizeof(drawn.scale));
    return true;
}

extern "C" FJELL_SDK_EXPORT void fjell_scene_teleport(uint32_t node_id, const float* position,
                                                      const float* rotation) {
    auto& services = fjell::script_services();
    if (!services.scene) return;
    auto* node = services.scene->find_spatial(node_id);
    if (!node) return;
    glm::vec3 world_position;
    glm::quat world_rotation;
    std::memcpy(&world_position, position, sizeof(world_position));
    std::memcpy(&world_rotation, rotation, sizeof(world_rotation));
    services.scene->teleport(*node, world_position, world_rotation);
    // A body's physics would otherwise put the node back where it was.
    if (services.physics) services.physics->teleport(node_id, world_position, world_rotation);
}

// The id of the component type scripts know by `type_name`. The library's
// own script types come first: a script type may share a name only with an
// engine component the library has no mirror of (the load refuses the
// others), so to that library the name means its script. The answer holds
// for as long as the library is loaded, and the SDK asks once per type.
extern "C" FJELL_SDK_EXPORT uint32_t fjell_component_type_id(const char* type_name) {
    if (!type_name) return FJELL_NO_COMPONENT_TYPE;
    if (auto* manager = fjell::script_services().manager) {
        if (const auto* type = manager->find_script_type(type_name)) return type->component.id;
    }
    if (auto id = fjell::ComponentRegistry::instance().find_id_by_script_name(type_name)) return *id;
    return FJELL_NO_COMPONENT_TYPE;
}

// The component of type `id` on one node, engine or script, or null.
static void* component_on(fjell::Node& node, fjell::ComponentTypeId id) {
    if (void* component = node.components.get_by_id(id)) return component;
    for (auto& entry : node.scripts) {
        if (entry.instance && entry.instance.type_id() == id) return entry.instance.get();
    }
    return nullptr;
}

extern "C" FJELL_SDK_EXPORT void* fjell_scene_get_component(uint32_t node_id, uint32_t type_id) {
    auto* scene = fjell::script_services().scene;
    if (!scene) return nullptr;
    auto* node = scene->find(node_id);
    if (!node) return nullptr;
    return component_on(*node, type_id);
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_find_node_with(uint32_t type_id) {
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

extern "C" FJELL_SDK_EXPORT void* fjell_scene_add_component(uint32_t node_id, uint32_t type_id) {
    auto& services = fjell::script_services();
    if (!services.scene) return nullptr;
    auto* node = services.scene->find(node_id);
    if (!node) return nullptr;

    if (const auto* type = services.manager ? services.manager->find_script_type_by_id(type_id)
                                            : nullptr) {
        auto* spatial = node->as_spatial();
        if (!spatial) return nullptr;
        fjell::ScriptComponent* made = services.manager->add_script(*spatial, type->name);
        services.scene->mark_structure_changed();
        return made;
    }

    const auto* meta = fjell::ComponentRegistry::instance().find(type_id);
    return meta && meta->script_add ? meta->script_add(*node) : nullptr;
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_scene_spawn_preset(uint32_t handle_id,
                                                              float x, float y, float z,
                                                              int32_t parent_id) {
    auto& s = fjell::script_services();
    if (!s.scene || !s.resources) return 0;

    auto* asset = s.resources->get(fjell::PresetAssetHandle{handle_id});
    if (!asset) return 0;

    fjell::Transform offset;
    offset.position = {x, y, z};
    return asset->preset.instantiate(*s.scene, parent_id, offset);
}
