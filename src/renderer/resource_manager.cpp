#include "renderer/resource_manager.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/primitives.hpp"
#include "renderer/texture.hpp"
#include "core/log.hpp"

#include <array>

namespace fjell {

// ── Pool implementation ─────────────────────────────────────────────────

template <typename T, typename HandleT>
HandleT ResourceManager::Pool<T, HandleT>::insert(const std::string& key,
                                                   std::unique_ptr<T> resource) {
    uint32_t index = static_cast<uint32_t>(slots.size());
    uint32_t gen = 1;
    slots.push_back(std::move(resource));
    generations.push_back(gen);
    auto handle = HandleT::make(index, gen);
    if (!key.empty()) {
        name_to_handle.emplace(key, handle);
    }
    return handle;
}

template <typename T, typename HandleT>
T* ResourceManager::Pool<T, HandleT>::get(HandleT handle) const {
    if (!handle.valid()) return nullptr;
    uint32_t idx = handle.index();
    if (idx >= slots.size()) return nullptr;
    if (generations[idx] != handle.generation()) return nullptr;
    return slots[idx].get();
}

// Explicit instantiations
template struct ResourceManager::Pool<Model, ModelHandle>;
template struct ResourceManager::Pool<Texture, TextureHandle>;
template struct ResourceManager::Pool<Material, MaterialHandle>;

// ── ResourceManager ─────────────────────────────────────────────────────

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
    const std::array<uint8_t, 4> flat_normal = {128, 128, 255, 255};
    const std::array<uint8_t, 4> default_mr_pixels = {255, 255, 0, 255};
    const std::array<uint8_t, 4> black = {0, 0, 0, 255};

    default_white_handle_ = textures_.insert("__default_white",
        std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_,
                                  white.data(), 1, 1));
    default_normal_handle_ = textures_.insert("__default_normal",
        std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_,
                                  flat_normal.data(), 1, 1));
    default_mr_handle_ = textures_.insert("__default_mr",
        std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_,
                                  default_mr_pixels.data(), 1, 1));
    default_black_handle_ = textures_.insert("__default_black",
        std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_,
                                  black.data(), 1, 1));
}

ModelHandle ResourceManager::load_model(const std::string& path) {
    auto it = models_.name_to_handle.find(path);
    if (it != models_.name_to_handle.end()) {
        FJELL_GFX_DEBUG("Model cache hit: {}", path);
        return it->second;
    }

    auto model = std::make_unique<Model>(device_, physical_device_, command_pool_, queue_, path);
    return models_.insert(path, std::move(model));
}

ModelHandle ResourceManager::create_primitive(const std::string& type) {
    std::string key = "primitive:" + type;
    auto it = models_.name_to_handle.find(key);
    if (it != models_.name_to_handle.end()) {
        return it->second;
    }

    PrimitiveMeshData data;
    if (type == "cube") {
        data = primitives::cube();
    } else if (type == "plane") {
        data = primitives::plane();
    } else if (type == "sphere") {
        data = primitives::sphere();
    } else {
        return {};
    }

    auto model = std::make_unique<Model>(device_, physical_device_, command_pool_, queue_, data);
    return models_.insert(key, std::move(model));
}

TextureHandle ResourceManager::load_texture(const std::string& path) {
    auto it = textures_.name_to_handle.find(path);
    if (it != textures_.name_to_handle.end()) {
        FJELL_GFX_DEBUG("Texture cache hit: {}", path);
        return it->second;
    }

    auto texture = std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_, path);
    return textures_.insert(path, std::move(texture));
}

MaterialHandle ResourceManager::create_material(const std::string& name,
                                                 TextureHandle base_color,
                                                 TextureHandle normal_map,
                                                 TextureHandle metallic_roughness,
                                                 TextureHandle emissive,
                                                 PbrParams params) {
    auto it = materials_.name_to_handle.find(name);
    if (it != materials_.name_to_handle.end()) {
        return it->second;
    }

    auto* bc  = base_color.valid()          ? get(base_color)          : get(default_white_handle_);
    auto* nm  = normal_map.valid()          ? get(normal_map)          : get(default_normal_handle_);
    auto* mr  = metallic_roughness.valid()  ? get(metallic_roughness)  : get(default_mr_handle_);
    auto* em  = emissive.valid()            ? get(emissive)            : get(default_black_handle_);

    auto material = std::make_unique<Material>(
        device_, material_pool_, material_layout_,
        *bc, *nm, *mr, *em, params);
    return materials_.insert(name, std::move(material));
}

MaterialHandle ResourceManager::create_material_from_texture(const std::string& name,
                                                              Texture* base_color,
                                                              PbrParams params) {
    auto it = materials_.name_to_handle.find(name);
    if (it != materials_.name_to_handle.end()) {
        return it->second;
    }

    auto* bc = base_color ? base_color : get(default_white_handle_);
    auto* nm = get(default_normal_handle_);
    auto* mr = get(default_mr_handle_);
    auto* em = get(default_black_handle_);

    auto material = std::make_unique<Material>(
        device_, material_pool_, material_layout_,
        *bc, *nm, *mr, *em, params);
    return materials_.insert(name, std::move(material));
}

Model* ResourceManager::get(ModelHandle handle) const {
    return models_.get(handle);
}

Texture* ResourceManager::get(TextureHandle handle) const {
    return textures_.get(handle);
}

Material* ResourceManager::get(MaterialHandle handle) const {
    return materials_.get(handle);
}

} // namespace fjell
