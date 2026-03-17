#include "renderer/resource_manager.hpp"
#include "renderer/bindless.hpp"
#include "renderer/material.hpp"
#include "renderer/model.hpp"
#include "renderer/primitives.hpp"
#include "renderer/texture.hpp"
#include "core/log.hpp"

namespace fjell {

// -- Pool implementation -----------------------------------------------------

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

// -- ResourceManager ---------------------------------------------------------

ResourceManager::ResourceManager(VkDevice device, VkPhysicalDevice physical_device,
                                 VkCommandPool command_pool, VkQueue queue,
                                 BindlessTextures& bindless)
    : device_{device}
    , physical_device_{physical_device}
    , command_pool_{command_pool}
    , queue_{queue}
    , bindless_{bindless} {
    // Create default 1x1 textures and register them in the bindless array.
    // These occupy bindless indices 0-3.
    const std::array<uint8_t, 4> white = {255, 255, 255, 255};
    const std::array<uint8_t, 4> flat_normal = {128, 128, 255, 255};
    const std::array<uint8_t, 4> default_mr_pixels = {255, 255, 0, 255};
    const std::array<uint8_t, 4> black = {0, 0, 0, 255};

    auto register_default = [&](const std::string& name, const uint8_t* pixels) -> TextureHandle {
        auto tex = std::make_unique<Texture>(device_, physical_device_, command_pool_, queue_,
                                             pixels, 1, 1);
        uint32_t bindless_idx = bindless_.add(*tex);
        auto handle = textures_.insert(name, std::move(tex));
        texture_to_bindless_[handle.index()] = bindless_idx;
        return handle;
    };

    default_white_handle_  = register_default("__default_white", white.data());
    default_normal_handle_ = register_default("__default_normal", flat_normal.data());
    default_mr_handle_     = register_default("__default_mr", default_mr_pixels.data());
    default_black_handle_  = register_default("__default_black", black.data());
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
    uint32_t bindless_idx = bindless_.add(*texture);
    auto handle = textures_.insert(path, std::move(texture));
    texture_to_bindless_[handle.index()] = bindless_idx;
    return handle;
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

    // Resolve texture handles to bindless indices (fall back to defaults)
    uint32_t bc_idx = bindless_index(base_color.valid()          ? base_color          : default_white_handle_);
    uint32_t nm_idx = bindless_index(normal_map.valid()          ? normal_map          : default_normal_handle_);
    uint32_t mr_idx = bindless_index(metallic_roughness.valid()  ? metallic_roughness  : default_mr_handle_);
    uint32_t em_idx = bindless_index(emissive.valid()            ? emissive            : default_black_handle_);

    // Build GPU material and register in SSBO
    auto material = std::make_unique<Material>(0, bc_idx, nm_idx, mr_idx, em_idx, params);
    uint32_t mat_id = bindless_.add_material(material->gpu_material());
    material = std::make_unique<Material>(mat_id, bc_idx, nm_idx, mr_idx, em_idx, params);

    return materials_.insert(name, std::move(material));
}

MaterialHandle ResourceManager::create_material_from_texture(const std::string& name,
                                                              Texture* base_color,
                                                              PbrParams params) {
    auto it = materials_.name_to_handle.find(name);
    if (it != materials_.name_to_handle.end()) {
        return it->second;
    }

    // Register the model-embedded texture in bindless (idempotent)
    uint32_t bc_idx = base_color ? bindless_.add(*base_color)
                                 : bindless_index(default_white_handle_);
    uint32_t nm_idx = bindless_index(default_normal_handle_);
    uint32_t mr_idx = bindless_index(default_mr_handle_);
    uint32_t em_idx = bindless_index(default_black_handle_);

    auto material = std::make_unique<Material>(0, bc_idx, nm_idx, mr_idx, em_idx, params);
    uint32_t mat_id = bindless_.add_material(material->gpu_material());
    material = std::make_unique<Material>(mat_id, bc_idx, nm_idx, mr_idx, em_idx, params);

    return materials_.insert(name, std::move(material));
}

uint32_t ResourceManager::bindless_index(TextureHandle handle) const {
    auto it = texture_to_bindless_.find(handle.index());
    if (it != texture_to_bindless_.end()) {
        return it->second;
    }
    return 0; // fallback to default white
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
