#pragma once

#include "renderer/mesh.hpp"
#include "renderer/texture.hpp"

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <vector>

namespace fjell {

struct PrimitiveMeshData;

class Model {
public:
    Model(VkDevice device, VkPhysicalDevice physical_device,
          VkCommandPool command_pool, VkQueue queue,
          const std::string& path);

    Model(VkDevice device, VkPhysicalDevice physical_device,
          VkCommandPool command_pool, VkQueue queue,
          const PrimitiveMeshData& data);

    void draw(VkCommandBuffer cmd) const;

    [[nodiscard]] const std::vector<std::unique_ptr<Texture>>& textures() const {
        return textures_;
    }
    [[nodiscard]] uint32_t mesh_count() const {
        return static_cast<uint32_t>(meshes_.size());
    }
    [[nodiscard]] uint32_t total_indices() const;

private:
    std::vector<std::unique_ptr<Mesh>> meshes_;
    std::vector<std::unique_ptr<Texture>> textures_;
};

} // namespace fjell
