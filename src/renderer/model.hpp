#pragma once

#include "renderer/mesh.hpp"
#include "renderer/texture.hpp"

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <vector>

namespace fjell {

class Model {
public:
    Model(VkDevice device, VkPhysicalDevice physical_device,
          VkCommandPool command_pool, VkQueue queue,
          const std::string& path);

    void draw(VkCommandBuffer cmd) const;

    [[nodiscard]] const std::vector<std::unique_ptr<Texture>>& textures() const {
        return textures_;
    }

private:
    std::vector<std::unique_ptr<Mesh>> meshes_;
    std::vector<std::unique_ptr<Texture>> textures_;
};

} // namespace fjell
