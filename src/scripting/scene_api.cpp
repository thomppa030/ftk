#include "scripting/script_globals.hpp"
#include "scripting/script_manager.hpp"
#include "scene/scene.hpp"
#include "physics/physics_types.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/sprite_anim_component.hpp"
#include "renderer/components/sprite_desc.hpp"

#include <string_view>

extern "C" uint32_t fjell_scene_find_node(const char* name) {
    auto* scene = fjell::script_active_scene();
    if (!scene || !name) return 0;
    for (const auto& obj : scene->objects()) {
        if (obj->name == name) return obj->id;
    }
    return 0;
}

extern "C" void fjell_scene_destroy(uint32_t node_id) {
    auto* scene = fjell::script_active_scene();
    if (scene) scene->queue_destroy(node_id);
}

extern "C" float* fjell_scene_get_position(uint32_t node_id) {
    auto* scene = fjell::script_active_scene();
    if (!scene) return nullptr;
    auto* node = scene->find_spatial(node_id);
    return node ? &node->transform.position.x : nullptr;
}

extern "C" float* fjell_scene_get_scale(uint32_t node_id) {
    auto* scene = fjell::script_active_scene();
    if (!scene) return nullptr;
    auto* node = scene->find_spatial(node_id);
    return node ? &node->transform.scale.x : nullptr;
}

extern "C" float* fjell_scene_get_rotation(uint32_t node_id) {
    auto* scene = fjell::script_active_scene();
    if (!scene) return nullptr;
    auto* node = scene->find_spatial(node_id);
    return node ? &node->transform.rotation.x : nullptr;
}

extern "C" void* fjell_scene_get_component(uint32_t node_id, const char* type_name) {
    auto* scene = fjell::script_active_scene();
    if (!scene || !type_name) return nullptr;
    auto* node = scene->find(node_id);
    if (!node) return nullptr;

    // Engine components
    std::string_view tn{type_name};
    if (tn == "RigidbodyComponent")
        return node->components.get<fjell::RigidbodyComponent>();
    if (tn == "ColliderComponent")
        return node->components.get<fjell::ColliderComponent>();
    if (tn == "CharacterComponent")
        return node->components.get<fjell::CharacterComponent>();
    if (tn == "MeshComponent")
        return node->components.get<fjell::MeshComponent>();
    if (tn == "SpriteComponent")
        return node->components.get<fjell::SpriteComponent>();
    if (tn == "SpriteAnimComponent")
        return node->components.get<fjell::SpriteAnimComponent>();

    // Script components
    for (auto& entry : node->scripts) {
        if (entry.instance && entry.type_name == type_name)
            return entry.instance.get();
    }
    return nullptr;
}

extern "C" uint32_t fjell_scene_spawn_empty(const char* name,
                                            float x, float y, float z) {
    auto* scene = fjell::script_active_scene();
    if (!scene) return 0;
    fjell::Transform t;
    t.position = {x, y, z};
    return scene->add_empty(t, name ? name : "Spawned");
}

extern "C" void* fjell_scene_add_component(uint32_t node_id,
                                           const char* type_name) {
    auto* scene = fjell::script_active_scene();
    if (!scene || !type_name) return nullptr;
    auto* node = scene->find(node_id);
    if (!node) return nullptr;

    // Engine components
    std::string_view tn{type_name};
    if (tn == "RigidbodyComponent")
        return &node->components.add<fjell::RigidbodyComponent>();
    if (tn == "ColliderComponent")
        return &node->components.add<fjell::ColliderComponent>();
    if (tn == "CharacterComponent")
        return &node->components.add<fjell::CharacterComponent>();
    if (tn == "MeshComponent")
        return &node->components.add<fjell::MeshComponent>();

    // Script components
    auto* mgr = fjell::script_active_manager();
    auto* spatial = node->as_spatial();
    if (!spatial || !mgr) return nullptr;
    auto result = mgr->add_script(*spatial, type_name);
    if (!result) return nullptr;
    auto& scripts = spatial->scripts;
    if (!scripts.empty() && scripts.back().instance)
        return scripts.back().instance.get();
    return nullptr;
}
