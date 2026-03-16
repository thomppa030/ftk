#pragma once

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
    Material* create_material(const std::string& name, Texture* texture,
                              glm::vec4 tint = {1.0f, 1.0f, 1.0f, 1.0f});

private:
    VkDevice device_;
    VkPhysicalDevice physical_device_;
    VkCommandPool command_pool_;
    VkQueue queue_;
    VkDescriptorPool material_pool_;
    VkDescriptorSetLayout material_layout_;

    std::unordered_map<std::string, std::unique_ptr<Model>> models_;
    std::unordered_map<std::string, std::unique_ptr<Texture>> textures_;
    std::unordered_map<std::string, std::unique_ptr<Material>> materials_;
};

} // namespace fjell
