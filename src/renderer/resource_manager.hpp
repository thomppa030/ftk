#pragma once

#include "renderer/uniform_types.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace fjell {

class Model;
class Texture;
class Material;

class ResourceManager {
public:
    ResourceManager(VkDevice device, VkPhysicalDevice physical_device,
                    VkCommandPool command_pool, VkQueue queue,
                    VkDescriptorPool material_pool,
                    VkDescriptorSetLayout material_layout);

    Model* load_model(const std::string& path);
    Model* create_primitive(const std::string& type);
    Texture* load_texture(const std::string& path);

    Material* create_material(const std::string& name,
                              Texture* base_color,
                              Texture* normal_map = nullptr,
                              Texture* metallic_roughness = nullptr,
                              Texture* emissive = nullptr,
                              PbrParams params = {});

    [[nodiscard]] Texture* default_white() const { return default_white_.get(); }
    [[nodiscard]] Texture* default_normal() const { return default_normal_.get(); }
    [[nodiscard]] Texture* default_metallic_roughness() const { return default_mr_.get(); }
    [[nodiscard]] Texture* default_black() const { return default_black_.get(); }

private:
    VkDevice device_;
    VkPhysicalDevice physical_device_;
    VkCommandPool command_pool_;
    VkQueue queue_;
    VkDescriptorPool material_pool_;
    VkDescriptorSetLayout material_layout_;

    // Default 1x1 textures for missing PBR maps
    std::unique_ptr<Texture> default_white_;
    std::unique_ptr<Texture> default_normal_;
    std::unique_ptr<Texture> default_mr_;
    std::unique_ptr<Texture> default_black_;

    std::unordered_map<std::string, std::unique_ptr<Model>> models_;
    std::unordered_map<std::string, std::unique_ptr<Texture>> textures_;
    std::unordered_map<std::string, std::unique_ptr<Material>> materials_;
};

} // namespace fjell
