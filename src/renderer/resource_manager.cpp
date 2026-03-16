#include "renderer/resource_manager.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/primitives.hpp"
#include "renderer/texture.hpp"
#include "core/log.hpp"

#include <array>

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
    // Create default 1x1 textures
    const std::array<uint8_t, 4> white = {255, 255, 255, 255};
    const std::array<uint8_t, 4> flat_normal = {128, 128, 255, 255}; // (0,0,1) in tangent space
    const std::array<uint8_t, 4> default_mr_pixels = {255, 255, 0, 255}; // R=ao(1), G=rough(1), B=metal(0)
    const std::array<uint8_t, 4> black = {0, 0, 0, 255};

    default_white_ = std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_,
                                                white.data(), 1, 1);
    default_normal_ = std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_,
                                                 flat_normal.data(), 1, 1);
    default_mr_ = std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_,
                                             default_mr_pixels.data(), 1, 1);
    default_black_ = std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_,
                                                black.data(), 1, 1);
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

Material* ResourceManager::create_material(const std::string& name,
                                            Texture* base_color,
                                            Texture* normal_map,
                                            Texture* metallic_roughness,
                                            Texture* emissive,
                                            PbrParams params) {
    auto it = materials_.find(name);
    if (it != materials_.end()) {
        return it->second.get();
    }

    auto material = std::make_unique<Material>(
        device_, material_pool_, material_layout_,
        base_color ? *base_color : *default_white_,
        normal_map ? *normal_map : *default_normal_,
        metallic_roughness ? *metallic_roughness : *default_mr_,
        emissive ? *emissive : *default_black_,
        params);
    auto* ptr = material.get();
    materials_.emplace(name, std::move(material));
    return ptr;
}

} // namespace fjell
