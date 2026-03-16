#include "renderer/resource_manager.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/primitives.hpp"
#include "renderer/texture.hpp"
#include "core/log.hpp"

namespace fjell {

ResourceManager::ResourceManager(VkDevice device, VkPhysicalDevice physical_device,
                                 VkCommandPool command_pool, VkQueue queue,
                                 VkDescriptorPool material_pool,
                                 VkDescriptorSetLayout material_layout)
    : device_{device}
    , physical_device_{physical_device}
    , command_pool_{command_pool}
    , queue_{queue}
    , material_pool_{material_pool}
    , material_layout_{material_layout} {
}

Model* ResourceManager::load_model(const std::string& path) {
    auto it = models_.find(path);
    if (it != models_.end()) {
        FJELL_GFX_DEBUG("Model cache hit: {}", path);
        return it->second.get();
    }

    auto model = std::make_unique<Model>(device_, physical_device_, command_pool_, queue_, path);
    auto* ptr = model.get();
    models_.emplace(path, std::move(model));
    return ptr;
}

Model* ResourceManager::create_primitive(const std::string& type) {
    std::string key = "primitive:" + type;
    auto it = models_.find(key);
    if (it != models_.end()) {
        return it->second.get();
    }

    PrimitiveMeshData data;
    if (type == "cube") {
        data = primitives::cube();
    } else if (type == "plane") {
        data = primitives::plane();
    } else if (type == "sphere") {
        data = primitives::sphere();
    } else {
        return nullptr;
    }

    auto model = std::make_unique<Model>(device_, physical_device_, command_pool_, queue_, data);
    auto* ptr = model.get();
    models_.emplace(key, std::move(model));
    return ptr;
}

Texture* ResourceManager::load_texture(const std::string& path) {
    auto it = textures_.find(path);
    if (it != textures_.end()) {
        FJELL_GFX_DEBUG("Texture cache hit: {}", path);
        return it->second.get();
    }

    auto texture = std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_, path);
    auto* ptr = texture.get();
    textures_.emplace(path, std::move(texture));
    return ptr;
}

Material* ResourceManager::create_material(const std::string& name, Texture* texture,
                                            glm::vec4 tint) {
    auto it = materials_.find(name);
    if (it != materials_.end()) {
        return it->second.get();
    }

    auto material = std::make_unique<Material>(device_, material_pool_, material_layout_,
                                                *texture, tint);
    auto* ptr = material.get();
    materials_.emplace(name, std::move(material));
    return ptr;
}

} // namespace fjell
